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

  def build_output
    result = ""
    i = 0
    while i < @out_lines.length
      line = @out_lines[i]
      if line == "/*TUPLE_INSERT_POINT*/"
        if @deferred_tuple != ""
          result << @deferred_tuple
        end
      elsif line == "/*LAMBDA_INSERT_POINT*/"
        if @deferred_lambda != ""
          result << @deferred_lambda
        end
      else
        result << line
        result << 10.chr
      end
      i = i + 1
    end
    result + ""
  end

  def initialize
    @out_lines = "".split(",")
    @out = ""
    @deferred_tuple = ""
    @deferred_lambda = ""
    @indent = 0
    @temp_counter = 0
    @label_counter = 0

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

    @nd_count = 0
    @root_id = 0

    # ---- Top-level methods (parallel arrays) ----
    @meth_names = "".split(",")
    @meth_param_names = "".split(",")
    @meth_param_types = "".split(",")
    # Per-param "deferred element" flag: "1" means at least one caller
    # passed an empty `[]` literal (or a local that itself was assigned
    # an empty literal). Used by the param body-push promotion pass
    # (issue #58) to decide whether the param's int_array can be safely
    # promoted to a concrete typed-array based on body usage.
    @meth_param_empty = "".split(",")
    @meth_return_types = "".split(",")
    @meth_body_ids = []
    @meth_has_defaults = "".split(",")

    # ---- Classes (parallel arrays) ----
    @cls_names = "".split(",")
    @cls_parents = "".split(",")
    @cls_ivar_names = "".split(",")
    @cls_ivar_types = "".split(",")
    @cls_meth_names = "".split(",")
    @cls_meth_params = "".split(",")
    @cls_meth_ptypes = "".split(",")
    @cls_meth_returns = "".split(",")
    @cls_meth_bodies = "".split(",")
    @cls_meth_defaults = "".split(",")
    # Mirror of @meth_param_empty for class methods. Pipe-separated by
    # method, comma-separated by param. Issue #58.
    @cls_meth_ptypes_empty = "".split(",")
    @cls_attr_readers = "".split(",")
    @cls_attr_writers = "".split(",")
    @cls_cmeth_names = "".split(",")
    @cls_cmeth_params = "".split(",")
    @cls_cmeth_ptypes = "".split(",")
    @cls_cmeth_returns = "".split(",")
    @cls_cmeth_bodies = "".split(",")
    @cls_is_value_type = []
    # SRA (scalar replacement of aggregates) eligibility flag per class.
    # Classes marked here can have their non-escaping instances replaced
    # with individual scalar locals. Distinct from value_type: SRA allows
    # attr_writer (mutation is rewritten to per-field assignment).
    @cls_is_sra = []

    # ---- Constants (parallel arrays) ----
    @const_names = "".split(",")
    @const_types = "".split(",")
    @const_expr_ids = []
    @const_scope_names = "".split(",")

    # ---- Scope stack for local variables ----
    @scope_names = "".split(",")
    @scope_types = "".split(",")

    @current_class_idx = -1
    @current_method_name = ""
    @current_lexical_scope = ""
    @current_method_return = ""
    @current_method_block_param = ""
    @in_main = 0
    @in_loop = 0
    @hoisted_strlen_var = ""
    @hoisted_strlen_recv = ""
    @in_yield_method = 0
    @in_gc_scope = 0

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
    @needs_mutable_str = 0
    @needs_rb_value = 0
    @needs_regexp = 0
    @needs_rand = 0
    @regexp_patterns = "".split(",")
    @regexp_flags = "".split(",")
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
    @pending_method_ref = ""
    @lambda_counter = 0
    @lambda_funcs = ""
    @lambda_params = "".split(",")
    @lambda_captures = "".split(",")
    @lambda_insert_pos = 0

    # Proc closure support (Phase 2)
    @in_proc_body = 0
    @proc_captures = "".split(",")
    @proc_capture_types = "".split(",")

    # Symbol type Phase 2 Step 1: intern table (infrastructure only; unused yet).
    @sym_names = "".split(",")
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

  def new_temp
    @temp_counter = @temp_counter + 1
    "_t" + @temp_counter.to_s
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
  def has_literal_block(nid)
    blk = @nd_block[nid]
    (blk >= 0 && @nd_type[blk] == "BlockNode") ? 1 : 0
  end

  # Returns the inner expression of a BlockArgumentNode whose payload
  # is a captured proc local (the `&block` form). Returns -1 for
  # absent block-arg, or for shapes the codegen doesn't yet forward
  # — `&:sym` (SymbolNode) and `&nil` (NilNode), which would need
  # symbol-to-proc / nil-as-no-block lowering. Call sites fall
  # through to the no-block path in those cases.
  def find_block_arg(nid)
    blk = @nd_block[nid]
    if blk < 0
      return -1
    end
    if @nd_type[blk] != "BlockArgumentNode"
      return -1
    end
    inner = @nd_expression[blk]
    if inner < 0
      return -1
    end
    if @nd_type[inner] != "LocalVariableReadNode"
      return -1
    end
    inner
  end

  # Resolves the call-site block-forwarding expression: returns the C
  # expression for the proc to forward at a `&block`-taking call site
  # (a literal block compiles to sp_proc_new(...); a `&proc_var` is
  # the captured `sp_Proc *` local), or "" if the call site provides
  # no block. Sets @needs_proc when a forwarded proc is emitted.
  def block_forward_expr(nid)
    if has_literal_block(nid) == 1
      @needs_proc = 1
      return compile_proc_literal(nid)
    end
    # Anonymous `&` forwarding (Ruby 3.1+): `inner(&)` where the
    # enclosing method declared `def outer(&)`. The BlockArgumentNode
    # carries no expression, so `find_block_arg` returns -1; we forward
    # the enclosing method's anon-block param directly.
    blk = @nd_block[nid]
    if blk >= 0 && @nd_type[blk] == "BlockArgumentNode" && @nd_expression[blk] < 0
      if @current_method_block_param != ""
        @needs_proc = 1
        return "lv_" + @current_method_block_param
      end
    end
    ba = find_block_arg(nid)
    if ba >= 0
      @needs_proc = 1
      return compile_expr(ba)
    end
    ""
  end

  # Flatten a constant reference into an internal name.
  #   C       -> C
  #   ::C     -> C
  #   M::C    -> M_C
  #   A::B::C -> A_B_C
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
  def is_known_constant_name(name)
    if const_namespace_exists(name) == 1
      return 1
    end
    if name == "ARGV" || name == "ENV" || name == "STDIN" || name == "STDOUT" || name == "STDERR"
      return 1
    end
    if name == "Math" || name == "File" || name == "Dir" || name == "Time" || name == "IO" || name == "Process" || name == "Kernel" || name == "Comparable" || name == "Enumerable"
      return 1
    end
    if name == "Object" || name == "Integer" || name == "String" || name == "Float" || name == "Symbol" || name == "Array" || name == "Hash" || name == "Range" || name == "Numeric" || name == "TrueClass" || name == "FalseClass" || name == "NilClass" || name == "Proc" || name == "Lambda" || name == "Regexp" || name == "MatchData" || name == "StringIO" || name == "Fiber"
      return 1
    end
    # Common exception classes referenced by raise / rescue. We
    # don't model the exception hierarchy beyond name-tagging.
    if name == "StandardError" || name == "RuntimeError" || name == "ArgumentError" || name == "TypeError" || name == "NameError" || name == "NoMethodError" || name == "IndexError" || name == "KeyError" || name == "ZeroDivisionError" || name == "FloatDomainError" || name == "RangeError" || name == "IOError" || name == "Errno" || name == "NotImplementedError" || name == "StopIteration" || name == "RegexpError" || name == "FrozenError" || name == "LocalJumpError" || name == "Exception"
      return 1
    end
    0
  end

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
      if cls_idx >= 0
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
    if idx < 0
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
    0
  end

  def pop_scope
    while @scope_names.length > 0
      top_name = @scope_names.last
      if top_name == "---"
        @scope_names.pop
        @scope_types.pop
        return
      end
      @scope_names.pop
      @scope_types.pop
    end
  end

  def declare_var(name, vtype)
    @scope_names.push(name)
    @scope_types.push(vtype)
    0
  end

  def find_var_type(name)
    i = @scope_names.length - 1
    while i >= 0
      if @scope_names[i] == name
        return @scope_types[i]
      end
      i = i - 1
    end
    ""
  end

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
  def find_regexp_index(nid)
    if @nd_type[nid] == "RegularExpressionNode"
      pat = @nd_unescaped[nid]
      i = 0
      while i < @regexp_patterns.length
        if @regexp_patterns[i] == pat
          return i
        end
        i = i + 1
      end
      return -1
    end
    # A constant initialized to a regex literal forwards to the
    # underlying pattern, so `RX = /pat/; RX.match?(s)` and
    # `s =~ RX` dispatch to the engine instead of falling through
    # to the literal-`(-1)` / `sp_str_include` fallbacks.
    if @nd_type[nid] == "ConstantReadNode"
      cname = resolve_const_ref_name(nid)
      if cname != ""
        ci = find_const_idx(cname)
        if ci >= 0 && ci < @const_expr_ids.length
          eid = @const_expr_ids[ci]
          if eid >= 0 && @nd_type[eid] == "RegularExpressionNode"
            return find_regexp_index(eid)
          end
        end
      end
    end
    # A local variable with exactly one write to a regex literal is
    # also resolvable. Multi-write or non-regex-write names were
    # marked ambiguous (-1) by scan_features.
    if @nd_type[nid] == "LocalVariableReadNode"
      lname = @nd_name[nid]
      i = 0
      while i < @local_regex_names.length
        if @local_regex_names[i] == lname
          return @local_regex_idx[i]
        end
        i = i + 1
      end
    end
    -1
  end

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

  # If the constant's initializer is a simple literal, return the
  # corresponding C expression.  Otherwise return "" so callers fall
  # back to cst_<name> lookup.  Enables propagation of:
  #   N = 10  →  10 at use sites
  #   PI = 3.14  →  3.14
  #   GREETING = "hi"  →  ("\xff" "hi" + 1)
  #   OK = true  →  TRUE
  def const_literal_c_value(ci)
    if ci < 0 || ci >= @const_expr_ids.length
      return ""
    end
    eid = @const_expr_ids[ci]
    if eid < 0
      return ""
    end
    et = @nd_type[eid]
    if et == "IntegerNode"
      return @nd_value[eid].to_s
    end
    if et == "FloatNode"
      return @nd_content[eid]
    end
    if et == "StringNode"
      return c_string_literal(@nd_content[eid])
    end
    if et == "TrueNode"
      return "TRUE"
    end
    if et == "FalseNode"
      return "FALSE"
    end
    if et == "NilNode"
      return "0"
    end
    if et == "SymbolNode"
      return compile_symbol_literal(@nd_content[eid])
    end
    ""
  end

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

  # Get method return type from class
  def cls_method_return(ci, mname)
    names = @cls_meth_names[ci].split(";")
    returns = @cls_meth_returns[ci].split(";")
    j = 0
    while j < names.length
      if names[j] == mname
        if j < returns.length
          return returns[j]
        end
        return "int"
      end
      j = j + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return cls_method_return(pi, mname)
      end
    end
    "int"
  end

  # Get ivar type from class
  def cls_ivar_type(ci, iname)
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    j = 0
    while j < names.length
      if names[j] == iname
        if j < types.length
          return types[j]
        end
        return "int"
      end
      j = j + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return cls_ivar_type(pi, iname)
      end
    end
    "int"
  end

  # ---- Emit helpers ----
  def emit(s)
    ind = ""
    j = 0
    while j < @indent
      ind = ind + "  "
      j = j + 1
    end
    @out_lines.push(ind + s)
  end

  def emit_raw(s)
    @out_lines.push(s)
  end


  # ---- Type inference ----
  def infer_type(nid)
    if nid < 0
      return "void"
    end
    t = @nd_type[nid]
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
    if t == "NumberedReferenceReadNode"
      return "string"
    end
    if t == "MatchWriteNode"
      return "int"
    end
    if t == "InterpolatedStringNode"
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
      return infer_hash_val_type(nid)
    end
    if t == "RangeNode"
      return "range"
    end
    if t == "LocalVariableReadNode"
      vt = find_var_type(@nd_name[nid])
      if vt != ""
        return vt
      end
      return "int"
    end
    if t == "GlobalVariableReadNode"
      gname = @nd_name[nid]
      gi = 0
      while gi < @gvar_names.length
        if @gvar_names[gi] == gname
          return @gvar_types[gi]
        end
        gi = gi + 1
      end
      return "int"
    end
    if t == "InstanceVariableReadNode"
      if @current_class_idx >= 0
        return cls_ivar_type(@current_class_idx, @nd_name[nid])
      end
      return "int"
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
        return "class_" + rname
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
          return "class_" + cpname
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
      return infer_type(@nd_left[nid])
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
    elems = parse_id_list(@nd_elements[nid])
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

  def infer_hash_val_type(nid)
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


  def infer_call_type(nid)
    mname = @nd_name[nid]
    recv = @nd_receiver[nid]

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

    # Bare method call in class context
    if @current_class_idx >= 0
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

  def infer_method_name_type(nid, mname, recv)
    if mname == "length"
      return "int"
    end
    if mname == "to_s"
      return "string"
    end
    if mname == "inspect"
      return "string"
    end
    if mname == "to_i"
      return "int"
    end
    if mname == "to_f"
      return "float"
    end
    if mname == "ceil"
      return "int"
    end
    if mname == "floor"
      return "int"
    end
    if mname == "round"
      return "int"
    end
    if mname == "upcase"
      return "string"
    end
    if mname == "downcase"
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
      return "int"
    end
    if mname == "strip"
      return "string"
    end
    if mname == "chomp"
      return "string"
    end
    if mname == "include?"
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
    if mname == "zero?"
      return "bool"
    end
    if mname == "frozen?"
      return "bool"
    end
    if mname == "is_a?"
      return "bool"
    end
    if mname == "respond_to?"
      return "bool"
    end
    if mname == "chr"
      return "string"
    end
    if mname == "gcd"
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
        if rt == "str_str_hash" || rt == "sym_str_hash" || rt == "int_str_hash"
          return "string"
        end
      end
      return "int"
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
      return "int"
    end
    if mname == "size"
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
      return "int"
    end
    if mname == "keys"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "int_str_hash"
          return "int_array"
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
    if mname == "fdiv"
      return "float"
    end
    if mname == "nan?" || mname == "finite?"
      return "bool"
    end
    if mname == "infinite?"
      return "int"
    end
    if mname == "truncate"
      return "int"
    end
    if mname == "tally"
      return "str_int_hash"
    end
    if mname == "values"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_str_hash" || rt == "int_str_hash"
          return "str_array"
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
            end
          end
        end
        return infer_type(recv)
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
        rt = infer_type(recv)
        if rt == "string"
          return "string"
        end
        if rt == "mutable_str"
          return "string"
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
        if rt == "argv"
          return "string"
        end
        if rt == "lambda"
          return "lambda"
        end
      end
      return "int"
    end
    ""
  end

  def infer_constructor_type(nid, mname, recv)
    if mname == "new"
      if recv >= 0
        rn = constructor_class_name(recv)
        if rn != ""
          if rn == "Array"
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
        if rcname == "File"
          if mname == "read"
            return "string"
          end
          if mname == "exist?"
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
        end
        if rcname == "Dir"
          if mname == "home"
            return "string"
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
          cmnames = @cls_cmeth_names[ci2].split(";")
          cm_returns = @cls_cmeth_returns[ci2].split(";")
          cj = 0
          while cj < cmnames.length
            if cmnames[cj] == mname
              if cj < cm_returns.length && cm_returns[cj] != "" && cm_returns[cj] != "int"
                return cm_returns[cj]
              end
            end
            cj = cj + 1
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
      end
      return "int_array"
    end
    ""
  end

  def infer_recv_method_type(nid, mname, recv)
    # Method call on poly
    if recv >= 0
      rt = infer_type(recv)
      if rt == "poly"
        if mname == "nil?"
          return "bool"
        end
        # Scan every user class that defines this method. If they all
        # agree on the return type, the call has that concrete type.
        # If they disagree, the call is genuinely polymorphic.
        return poly_dispatch_return_type(mname)
      end
      # Method call on int (possible IntArray element storing object pointers)
      if rt == "int"
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


  def is_class_or_ancestor(cname, target)
    if cname == target
      return 1
    end
    ci = find_class_idx(cname)
    if ci >= 0
      if @cls_parents[ci] != ""
        return is_class_or_ancestor(@cls_parents[ci], target)
      end
    end
    0
  end

  def is_operator_name(name)
    if name == "+"
      return 1
    end
    if name == "-"
      return 1
    end
    if name == "*"
      return 1
    end
    if name == "/"
      return 1
    end
    if name == "%"
      return 1
    end
    if name == "<"
      return 1
    end
    if name == ">"
      return 1
    end
    if name == "<="
      return 1
    end
    if name == ">="
      return 1
    end
    if name == "=="
      return 1
    end
    if name == "!="
      return 1
    end
    if name == "<=>"
      return 1
    end
    0
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

  def tuple_arity(t)
    tuple_elem_types_str(t).split(",").length
  end

  def tuple_c_name(t)
    # "tuple:int,string" → "sp_Tuple_int_string"
    "sp_Tuple_" + tuple_elem_types_str(t).split(",").join("_")
  end

  # Whether a tuple element type must be traced by the GC scan function.
  # Scalars (int/float/bool/symbol) are pure values; pointer-to-GC-object
  # element types must be marked, otherwise the GC frees the inner object
  # while the tuple keeps a dangling pointer.
  def tuple_field_needs_mark(et)
    if et == "poly"
      return 1
    end
    if et == "int" || et == "float" || et == "bool" || et == "symbol" || et == "void" || et == "nil"
      return 0
    end
    type_is_pointer(et)
  end

  # Returns the scan function name for the tuple, or "NULL" if no field
  # requires marking.
  def tuple_scan_name(t)
    parts = tuple_elem_types_str(t).split(",")
    fi = 0
    while fi < parts.length
      if tuple_field_needs_mark(parts[fi]) == 1
        return tuple_c_name(t) + "_scan"
      end
      fi = fi + 1
    end
    "NULL"
  end

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

  # Inferred C type of the i-th lvalue in `a, b, c = rhs`.  Tuple RHS gives
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
    if t == "sym_array"
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
  def expr_may_gc(nid)
    if nid < 0
      return 0
    end
    t = @nd_type[nid]
    if t == "IntegerNode" || t == "FloatNode" || t == "StringNode"
      return 0
    end
    if t == "SymbolNode" || t == "TrueNode" || t == "FalseNode" || t == "NilNode"
      return 0
    end
    if t == "LocalVariableReadNode" || t == "SelfNode"
      return 0
    end
    if t == "InstanceVariableReadNode" || t == "ConstantReadNode"
      return 0
    end
    1
  end

  def is_nullable_type(t)
    if t.length > 1 && t[t.length - 1] == "?"
      return 1
    end
    0
  end

  # Issue #58: empty `[]` literal needs deferred element-type
  # resolution. This helper distinguishes `[]` from `[1, 2, 3]` so the
  # promotion machinery can know "writes haven't fixed the element type
  # yet, so a later push can still pick it".
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

  def base_type(t)
    if t.length > 1 && t[t.length - 1] == "?"
      return t[0, t.length - 1]
    end
    t
  end

  def is_nullable_pointer_type(t)
    # Pointer types that can represent nil as NULL
    bt = base_type(t)
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

  def is_value_type_obj(t)
    if is_obj_type(t) == 1
      cname = t[4, t.length - 4]
      ci = find_class_idx(cname)
      if ci >= 0 && @cls_is_value_type[ci] == 1
        return 1
      end
    end
    0
  end

  # ---- C type mapping ----
  def c_type(t)
    if is_nullable_type(t) == 1
      t = base_type(t)
    end
    if t == "range"
      return "sp_Range"
    end
    if t == "int"
      return "mrb_int"
    end
    if t == "bigint"
      return "sp_Bigint *"
    end
    if t == "float"
      return "mrb_float"
    end
    if t == "bool"
      return "mrb_bool"
    end
    if t == "string"
      return "const char *"
    end
    if t == "symbol"
      return "sp_sym"
    end
    if t == "mutable_str"
      return "sp_String *"
    end
    if t == "void"
      return "mrb_int"
    end
    if t == "nil"
      return "mrb_int"
    end
    if t == "int_array"
      return "sp_IntArray *"
    end
    if t == "float_array"
      return "sp_FloatArray *"
    end
    if is_ptr_array_type(t) == 1
      return "sp_PtrArray *"
    end
    if t == "str_array"
      return "sp_StrArray *"
    end
    if t == "str_int_hash"
      return "sp_StrIntHash *"
    end
    if t == "str_str_hash"
      return "sp_StrStrHash *"
    end
    if t == "int_str_hash"
      return "sp_IntStrHash *"
    end
    if t == "sym_int_hash"
      return "sp_SymIntHash *"
    end
    if t == "sym_str_hash"
      return "sp_SymStrHash *"
    end
    if t == "str_poly_hash"
      return "sp_StrPolyHash *"
    end
    if t == "sym_poly_hash"
      return "sp_SymPolyHash *"
    end
    if t == "sym_array"
      # sym_array is an IntArray internally (sp_sym = mrb_int)
      return "sp_IntArray *"
    end
    if is_tuple_type(t) == 1
      return tuple_c_name(t) + " *"
    end
    if t == "fiber"
      return "sp_Fiber *"
    end
    if t == "poly"
      return "sp_RbVal"
    end
    if t == "proc"
      return "sp_Proc *"
    end
    if t == "stringio"
      return "sp_StringIO *"
    end
    if t == "poly_array"
      return "sp_PolyArray *"
    end
    if t == "lambda"
      return "sp_Val *"
    end
    if is_obj_type(t) == 1
      cname = t[4, t.length - 4]
      ci = find_class_idx(cname)
      if ci >= 0 && @cls_is_value_type[ci] == 1
        return "sp_" + cname
      end
      return "sp_" + cname + " *"
    end
    "mrb_int"
  end

  def c_default_val(t)
    if is_nullable_type(t) == 1
      return "NULL"
    end
    # NOTE: nullable returns above, so rest handles base types only
    if t == "range"
      return "((sp_Range){0,0})"
    end
    if t == "int"
      return "0"
    end
    if t == "bigint"
      return "NULL"
    end
    if t == "float"
      return "0.0"
    end
    if t == "bool"
      return "FALSE"
    end
    if t == "string"
      return "(&(\"\\xff\")[1])"
    end
    if t == "symbol"
      return "((sp_sym)-1)"
    end
    if t == "mutable_str"
      return "NULL"
    end
    if t == "void"
      return "0"
    end
    if t == "nil"
      return "0"
    end
    if t == "poly"
      return "sp_box_nil()"
    end
    if t == "stringio"
      return "NULL"
    end
    if t == "proc"
      return "NULL"
    end
    if type_is_pointer(t) == 1
      return "NULL"
    end
    if is_value_type_obj(t) == 1
      return "{0}"
    end
    "0"
  end

  def c_return_default(t)
    # Like c_default_val but for return statements (compound literal for value types)
    if is_value_type_obj(t) == 1
      return "(" + c_type(t) + "){0}"
    end
    c_default_val(t)
  end

  # PM_RANGE_FLAGS_EXCLUDE_END = 4: bit 2 set means `...` (exclusive).
  def range_excl_end(rid)
    if (@nd_flags[rid] & 4) != 0
      return 1
    end
    return 0
  end

  def sanitize_name(name)
    if name == "<=>"
      return "_cmp"
    end
    if name == "<="
      return "_le"
    end
    if name == ">="
      return "_ge"
    end
    if name == "<<"
      return "_lshift"
    end
    if name == ">>"
      return "_rshift"
    end
    if name == "<"
      return "_lt"
    end
    if name == ">"
      return "_gt"
    end
    if name == "+"
      return "_plus"
    end
    if name == "-"
      return "_minus"
    end
    if name == "*"
      return "_mul"
    end
    if name == "/"
      return "_div"
    end
    if name == "=="
      return "_eq_eq"
    end
    if name == "!="
      return "_neq"
    end
    if name == "[]"
      return "_aref"
    end
    if name == "[]="
      return "_aset"
    end
    result = ""
    i = 0
    while i < name.length
      ch = name[i]
      if ch == "?"
        result = result + "_p"
      else
        if ch == "!"
          result = result + "_bang"
        else
          if ch == "="
            result = result + "_eq"
          else
            result = result + ch
          end
        end
      end
      i = i + 1
    end
    result
  end

  def sanitize_ivar(name)
    # @x → iv_x, x → iv_x
    if name.length > 0 && name[0] == "@"
      return "iv_" + name[1, name.length - 1]
    end
    "iv_" + name
  end

  def sanitize_gvar(name)
    # $last → gv_last, $1 → gv_1
    if name.length > 0 && name[0] == "$"
      return "gv_" + name[1, name.length - 1]
    end
    "gv_" + name
  end

  # ---- Array type helpers ----
  def array_c_prefix(t)
    if t == "str_array"
      return "StrArray"
    end
    if t == "float_array"
      return "FloatArray"
    end
    if t == "poly_array"
      return "PolyArray"
    end
    if is_ptr_array_type(t) == 1
      return "PtrArray"
    end
    "IntArray"
  end

  # The canonical "is this an array type?" check. Use this when you need
  # to dispatch a method that's defined for every typed array — `+`,
  # `concat`, `shuffle`, `each_with_object`, `flat_map`, etc. Covers the
  # 5 typed arrays (int/str/float/sym/poly) and any *_ptr_array.
  def is_array_type(t)
    if t == "int_array" || t == "str_array" || t == "float_array" || t == "sym_array" || t == "poly_array"
      return 1
    end
    if is_ptr_array_type(t) == 1
      return 1
    end
    0
  end

  # Set the right @needs_<runtime> flag for the given array type.
  def mark_array_runtime_needs(t)
    if t == "float_array"
      @needs_float_array = 1
    elsif t == "str_array"
      @needs_str_array = 1
    elsif t == "poly_array"
      @needs_rb_value = 1
    else
      @needs_int_array = 1
    end
  end

  # ---- Collection pass ----
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

    # Pass 2: top-level methods, constants, define_method
    stmts.each { |sid|
      if @nd_type[sid] == "DefNode"
        collect_toplevel_method(sid)
      end
      if @nd_type[sid] == "ConstantWriteNode"
        collect_constant(sid)
      end
      if @nd_type[sid] == "CallNode"
        if @nd_name[sid] == "define_method"
          collect_define_method(sid)
        end
      end
    }

    # Pass 2.5: infer lambda parameter types from call sites
    infer_lambda_param_types

    # Pass 3: infer return types
    infer_all_returns
  end

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

  def collect_class(nid)
    collect_class_with_prefix(nid, "")
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
                  collect_module_methods_into_class(ci, mod_name)
                end
                ik = ik + 1
              end
            end
          end
        end
      }
      collect_ivars(ci)
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
        parent = @nd_name[sp]
      end
    end

    ci = @cls_names.length
    @cls_names.push(cname)
    @cls_is_value_type.push(0)
    @cls_is_sra.push(0)
    @cls_parents.push(parent)
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
                collect_module_methods_into_class(ci, mod_name)
              end
              ik = ik + 1
            end
          end
        end
      end
    }

    # Collect ivars
    collect_ivars(ci)
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
        append_cls_cmeth(ci, mname, params_str, ptypes_str, "int", body_id)
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
      @cls_meth_ptypes[ci] = @cls_meth_ptypes[ci] + "|" + ptypes
      @cls_meth_returns[ci] = @cls_meth_returns[ci] + ";" + ret
      @cls_meth_bodies[ci] = @cls_meth_bodies[ci] + ";" + body_id.to_s
      @cls_meth_defaults[ci] = @cls_meth_defaults[ci] + "|" + defaults
      @cls_meth_ptypes_empty[ci] = @cls_meth_ptypes_empty[ci] + "|"
    else
      @cls_meth_names[ci] = name
      @cls_meth_params[ci] = params
      @cls_meth_ptypes[ci] = ptypes
      @cls_meth_returns[ci] = ret
      @cls_meth_bodies[ci] = body_id.to_s
      @cls_meth_defaults[ci] = defaults
      @cls_meth_ptypes_empty[ci] = ""
    end
  end

  def append_cls_cmeth(ci, name, params, ptypes, ret, body_id)
    if @cls_cmeth_names[ci] != ""
      @cls_cmeth_names[ci] = @cls_cmeth_names[ci] + ";" + name
      @cls_cmeth_params[ci] = @cls_cmeth_params[ci] + "|" + params
      @cls_cmeth_ptypes[ci] = @cls_cmeth_ptypes[ci] + "|" + ptypes
      @cls_cmeth_returns[ci] = @cls_cmeth_returns[ci] + ";" + ret
      @cls_cmeth_bodies[ci] = @cls_cmeth_bodies[ci] + ";" + body_id.to_s
    else
      @cls_cmeth_names[ci] = name
      @cls_cmeth_params[ci] = params
      @cls_cmeth_ptypes[ci] = ptypes
      @cls_cmeth_returns[ci] = ret
      @cls_cmeth_bodies[ci] = body_id.to_s
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
        end
        return
      end
      k = k + 1
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
          if old == "int" || old == "nil"
            types[k] = new_type
            @cls_ivar_types[ci] = types.join(";")
          elsif old != new_type && old != "poly"
            # Nullable pattern: nil + T → T?, T + nil → T?
            if new_type == "nil" && is_nullable_pointer_type(old) == 1
              if old[old.length - 1] != "?"
                types[k] = old + "?"
                @cls_ivar_types[ci] = types.join(";")
              end
            elsif old == "nil" && is_nullable_pointer_type(new_type) == 1
              types[k] = new_type + "?"
              @cls_ivar_types[ci] = types.join(";")
            else
              types[k] = "poly"
              @cls_ivar_types[ci] = types.join(";")
            end
          end
        end
        return
      end
      k = k + 1
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

  def add_ivar(ci, iname, itype)
    if @cls_ivar_names[ci] != ""
      @cls_ivar_names[ci] = @cls_ivar_names[ci] + ";" + iname
      @cls_ivar_types[ci] = @cls_ivar_types[ci] + ";" + itype
    else
      @cls_ivar_names[ci] = iname
      @cls_ivar_types[ci] = itype
    end
  end

  def scan_ivars(ci, nid)
    if nid < 0
      return
    end
    if @nd_type[nid] == "InstanceVariableWriteNode"
      iname = @nd_name[nid]
      if ivar_exists(ci, iname) == 0
        vtype = infer_ivar_init_type(@nd_expression[nid])
        add_ivar(ci, iname, vtype)
      else
        # Update type if current type is int (default/nil) and new type is better
        expr = @nd_expression[nid]
        if expr >= 0
          if @nd_type[expr] != "NilNode"
            vtype = infer_ivar_init_type(expr)
            if vtype != "int"
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
    # Recurse into children
    scan_ivars_children(ci, nid)
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

    body_stmts.each { |sid|
      if @nd_type[sid] == "ConstantWriteNode"
        collect_scoped_constant(mname, sid)
      end
      # Collect module class methods (def self.xxx) as top-level functions
      if @nd_type[sid] == "DefNode"
        if @nd_receiver[sid] >= 0
          if @nd_type[@nd_receiver[sid]] == "SelfNode"
            dmname = @nd_name[sid]
            # Create as top-level method with module prefix for dispatch
            @meth_names.push(mname + "_cls_" + dmname)
            @meth_param_names.push(collect_params_str(sid))
            @meth_param_types.push(collect_ptypes_str(sid, -1))
            @meth_param_empty.push("")
            @meth_return_types.push("int")
            @meth_body_ids.push(@nd_body[sid])
            @meth_has_yield.push(0)
            @meth_has_defaults.push("0")
          end
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
    }
  end

  def collect_constant(nid)
    collect_scoped_constant("", nid)
  end

  def collect_struct_class(cname, call_nid)
    # Generate a synthetic class from Struct.new(:field1, :field2, ...)
    ci = @cls_names.length
    @cls_names.push(cname)
    @cls_is_value_type.push(0)
    @cls_is_sra.push(0)
    @cls_parents.push("")
    @cls_ivar_names.push("")
    @cls_ivar_types.push("")
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
    if @nd_type[nid] == "BlockNode"
      # Don't look inside blocks of other calls for yield
      # Actually we do - yield inside a block still belongs to enclosing method
    end
    # Recurse children
    if @nd_body[nid] >= 0
      if body_has_yield(@nd_body[nid]) == 1
        return 1
      end
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      if body_has_yield(stmts[k]) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      if body_has_yield(@nd_expression[nid]) == 1
        return 1
      end
    end
    if @nd_predicate[nid] >= 0
      if body_has_yield(@nd_predicate[nid]) == 1
        return 1
      end
    end
    if @nd_subsequent[nid] >= 0
      if body_has_yield(@nd_subsequent[nid]) == 1
        return 1
      end
    end
    if @nd_else_clause[nid] >= 0
      if body_has_yield(@nd_else_clause[nid]) == 1
        return 1
      end
    end
    if @nd_left[nid] >= 0
      if body_has_yield(@nd_left[nid]) == 1
        return 1
      end
    end
    if @nd_right[nid] >= 0
      if body_has_yield(@nd_right[nid]) == 1
        return 1
      end
    end
    if @nd_block[nid] >= 0
      if body_has_yield(@nd_block[nid]) == 1
        return 1
      end
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      if body_has_yield(conds[k]) == 1
        return 1
      end
      k = k + 1
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      if body_has_yield(args[k]) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_arguments[nid] >= 0
      if body_has_yield(@nd_arguments[nid]) == 1
        return 1
      end
    end
    if @nd_receiver[nid] >= 0
      if body_has_yield(@nd_receiver[nid]) == 1
        return 1
      end
    end
    0
  end

  # ---- Return type inference ----
  def infer_constructor_types
    # Scan AST for ClassName.new(args) calls and infer param types
    scan_new_calls(@root_id)
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
    if at == "int"
      # Numeric compat: int + float is safe in both directions.
      if old_pt == "float"
        return "float"
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
    # Genuinely incompatible types: fall back to polymorphic value.
    @needs_rb_value = 1
    "poly"
  end

  def scan_new_calls(nid)
    if nid < 0
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
                            if ptypes[pi] == "int"
                              if at != "int"
                                ptypes[pi] = at
                              end
                            end
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
                        # Don't clobber the trailing rest int_array param.
                        if pi3 == ptypes.length - 1 && ptypes[pi3] == "int_array"
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
                    if ptypes[ak] == "int"
                      if at != "int"
                        ptypes[ak] = at
                      end
                    end
                  end
                end
              end
              ak = ak + 1
            end
            @meth_param_types[mi] = ptypes.join(",")
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
                    all_ptypes = @cls_meth_ptypes[init_ci].split("|")
                    all_params = @cls_meth_params[init_ci].split("|")
                    if init_idx < all_ptypes.length
                      ptypes = all_ptypes[init_idx].split(",")
                      pnames = "".split(",")
                      if init_idx < all_params.length
                        pnames = all_params[init_idx].split(",")
                      end
                      k = 0
                      while k < arg_ids.length
                        if @nd_type[arg_ids[k]] == "KeywordHashNode"
                          # Handle keyword args
                          elems = parse_id_list(@nd_elements[arg_ids[k]])
                          ek = 0
                          while ek < elems.length
                            if @nd_type[elems[ek]] == "AssocNode"
                              key_id = @nd_key[elems[ek]]
                              if key_id >= 0
                                kname = ""
                                if @nd_type[key_id] == "SymbolNode"
                                  kname = @nd_content[key_id]
                                end
                                expr_id = @nd_expression[elems[ek]]
                                at = infer_type(expr_id)
                                pi = 0
                                while pi < pnames.length
                                  if pnames[pi] == kname
                                    if pi < ptypes.length
                                      ptypes[pi] = unify_call_types(ptypes[pi], at, expr_id)
                                    end
                                  end
                                  pi = pi + 1
                                end
                              end
                            end
                            ek = ek + 1
                          end
                        else
                          at = infer_type(arg_ids[k])
                          if k < ptypes.length
                            ptypes[k] = unify_call_types(ptypes[k], at, arg_ids[k])
                          end
                        end
                        k = k + 1
                      end
                      all_ptypes[init_idx] = ptypes.join(",")
                      @cls_meth_ptypes[init_ci] = all_ptypes.join("|")
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
            midx = cls_find_method_direct(ci, mname)
            if midx >= 0
              args_id = @nd_arguments[nid]
              if args_id >= 0
                arg_ids = get_args(args_id)
                all_ptypes = @cls_meth_ptypes[ci].split("|")
                if midx < all_ptypes.length
                  ptypes = all_ptypes[midx].split(",")
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
                  all_ptypes[midx] = ptypes.join(",")
                  @cls_meth_ptypes[ci] = all_ptypes.join("|")
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
            all_params = @cls_meth_params[i].split("|")
            all_ptypes = @cls_meth_ptypes[i].split("|")
            pnames = "".split(",")
            ptypes = "".split(",")

            if init_idx2 < all_params.length
              pnames = all_params[init_idx2].split(",")
            end
            if init_idx2 < all_ptypes.length
              ptypes = all_ptypes[init_idx2].split(",")
            end
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
        all_params = @cls_meth_params[i].split("|")
        all_ptypes = @cls_meth_ptypes[i].split("|")
        pnames = "".split(",")
        ptypes = "".split(",")

        if init_idx < all_params.length
          pnames = all_params[init_idx].split(",")
        end
        if init_idx < all_ptypes.length
          ptypes = all_ptypes[init_idx].split(",")
        end
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
                              if ivar_types[ij] == "int"
                                ivar_types[ij] = ptypes[pi]
                              end
                              if ivar_types[ij] == "nil"
                                ivar_types[ij] = ptypes[pi]
                              end
                            end
                          end
                          ij = ij + 1
                        end
                        @cls_ivar_types[i] = ivar_types.join(";")
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
      all_params = @cls_meth_params[oci].split("|")
      all_ptypes = @cls_meth_ptypes[oci].split("|")
      bodies = @cls_meth_bodies[oci].split(";")
      j = 0
      while j < mnames.length
        if mnames[j] != "initialize"
          pnames = "".split(",")
          ptypes = "".split(",")

          if j < all_params.length
            pnames = all_params[j].split(",")
          end
          if j < all_ptypes.length
            ptypes = all_ptypes[j].split(",")
          end
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
                  # method actually called on this param. The old
                  # algorithm matched on a single reader and ignored
                  # later accesses, picking a class that didn't satisfy
                  # them — issue #35.
                  called = "".split(",")
                  collect_param_methods(bid, pnames[pk], called)
                  if called.length > 0
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
                      all_ptypes[j] = ptypes.join(",")
                      @cls_meth_ptypes[oci] = all_ptypes.join("|")
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
              if called.length > 0
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

  # Collect every method name called on `pname` anywhere under nid.
  # Used by parameter type inference to find the class that satisfies
  # ALL accesses, avoiding a single-reader match that ignores later
  # method calls on the same parameter.
  def collect_param_methods(nid, pname, acc)
    if nid < 0
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
  end

  # Issue #58: collect every element type seen in `pname.push(elem)`
  # or `pname << elem` patterns under nid. The deferred-element-type
  # promotion pass uses this to decide what concrete typed-array a
  # parameter should be promoted to when callers all passed empty
  # `[]` literals.
  def collect_param_push_elem_types(nid, pname, acc)
    if nid < 0
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

  # Issue #58: promote each top-level method parameter from int_array
  # to a concrete typed-array (str_array, float_array, sym_array)
  # when (a) every caller passed an empty `[]` literal — guarded by
  # @meth_param_empty[mi][k] == "1" — and (b) the body's pushes on
  # that parameter all agree on a single concrete element type.
  # Without (a), a caller passing a real int_array would be silently
  # miscompiled. Without (b), a body that pushes mixed types should
  # surface as a type error rather than picking one arbitrarily.
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
        end
        ci = ci + 1
      end
      @current_class_idx = -1
    end
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
    end
    ""
  end

  # Does class `ci` provide `mname` as a reader, writer, or method?
  # Walks parent classes for inherited members.
  def class_has_method(ci, mname)
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
        scan_writer_calls(@meth_body_ids[i])
      end
      pop_scope
      i = i + 1
    end
    # Scan class method bodies
    ci = 0
    while ci < @cls_names.length
      @current_class_idx = ci
      bodies = @cls_meth_bodies[ci].split(";")
      mnames = @cls_meth_names[ci].split(";")
      all_params = @cls_meth_params[ci].split("|")
      all_ptypes = @cls_meth_ptypes[ci].split("|")
      bj = 0
      while bj < bodies.length
        bid = bodies[bj].to_i
        if bid >= 0
          push_scope
          pnames2 = "".split(",")
          ptypes2 = "".split(",")
          if bj < all_params.length
            pnames2 = all_params[bj].split(",")
          end
          if bj < all_ptypes.length
            ptypes2 = all_ptypes[bj].split(",")
          end
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
          scan_writer_calls(bid)
          pop_scope
        end
        bj = bj + 1
      end
      ci = ci + 1
    end
    @current_class_idx = -1
    # Scan main-level code
    scan_writer_calls(@root_id)
    pop_scope
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
        # Empty `{}` / `[]` literal: don't reset the ivar's tracked
        # type to the default (`str_int_hash` / `int_array`), since a
        # later `[]=` write may have already promoted the slot to a
        # more specific type. Reseeding from the empty-default would
        # widen the promoted type to poly on the next iteration.
        if is_empty_hash_literal(expr_id) == 0 && is_empty_array_literal(expr_id) == 0
          at = infer_type(expr_id)
          if at != "int" && at != "nil"
            update_ivar_type(@current_class_idx, iname, at)
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
                  @needs_str_poly_hash = 1
                  @needs_rb_value = 1
                elsif promoted == "sym_poly_hash"
                  @needs_sym_poly_hash = 1
                  @needs_rb_value = 1
                end
              end
            end
          end
        end
      end
    end
    # Recurse
    if @nd_body[nid] >= 0
      scan_writer_calls(@nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_writer_calls(stmts[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_writer_calls(@nd_expression[nid])
    end
    if @nd_arguments[nid] >= 0
      scan_writer_calls(@nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_writer_calls(args[k])
      k = k + 1
    end
    if @nd_block[nid] >= 0
      scan_writer_calls(@nd_block[nid])
    end
    if @nd_predicate[nid] >= 0
      scan_writer_calls(@nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      scan_writer_calls(@nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      scan_writer_calls(@nd_else_clause[nid])
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
    # Update ivar types from constructor params
    update_ivar_types_from_params
    # Infer setter param types from ivar types
    infer_writer_param_types

    # Top-level methods
    i = 0
    while i < @meth_names.length
      push_scope
      # Open class self type
      mfn = @meth_names[i]
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
      # Also declare locals for better return type inference
      if @meth_body_ids[i] >= 0
        lnames = "".split(",")
        ltypes = "".split(",")
        scan_locals(@meth_body_ids[i], lnames, ltypes, pnames)
        lk = 0
        while lk < lnames.length
          declare_var(lnames[lk], ltypes[lk])
          lk = lk + 1
        end
        # Second pass: upgrade nil/int types with better information
        lnames2 = "".split(",")
        ltypes2 = "".split(",")
        scan_locals(@meth_body_ids[i], lnames2, ltypes2, pnames)
        lk = 0
        while lk < lnames2.length
          mk = 0
          while mk < lnames.length
            if lnames[mk] == lnames2[lk]
              if ltypes[mk] == "int" || ltypes[mk] == "nil"
                if ltypes2[lk] != "int" && ltypes2[lk] != "nil"
                  ltypes[mk] = ltypes2[lk]
                  set_var_type(lnames[mk], ltypes2[lk])
                end
              end
            end
            mk = mk + 1
          end
          lk = lk + 1
        end
      end
      rt = infer_body_return(@meth_body_ids[i])
      @meth_return_types[i] = rt
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
            #   existing_pt: from infer_constructor_types scanning Foo.new(...)
            #     call sites (already widened to "poly" via unify_call_types
            #     when call sites disagree).
            #   body_pt: from scanning the initialize body for `@x = param`
            #     ivar writes; "int" means "no info" (the fallback).
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
          scan_locals_first_type(bid, rlnames, rltypes, pnames)
          rlk = 0
          while rlk < rlnames.length
            declare_var(rlnames[rlk], rltypes[rlk])
            rlk = rlk + 1
          end
          # Second pass with locals in scope
          rlnames2 = "".split(",")
          rltypes2 = "".split(",")
          scan_locals_first_type(bid, rlnames2, rltypes2, pnames)
          rlk2 = 0
          while rlk2 < rlnames2.length
            if rltypes2[rlk2] != "int"
              set_var_type(rlnames2[rlk2], rltypes2[rlk2])
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
        # Save incrementally so later methods can see updated return types
        @cls_meth_returns[i] = returns.join(";")
        pop_scope
        j = j + 1
      end

      # Class methods
      cmnames = @cls_cmeth_names[i].split(";")
      cm_bodies = @cls_cmeth_bodies[i].split(";")
      cm_returns = @cls_cmeth_returns[i].split(";")
      j = 0
      while j < cmnames.length
        push_scope
        bid = -1
        if j < cm_bodies.length
          bid = cm_bodies[j].to_i
        end
        rt = infer_body_return(bid)
        if j < cm_returns.length
          cm_returns[j] = rt
        end
        pop_scope
        j = j + 1
      end
      @cls_cmeth_returns[i] = cm_returns.join(";")
      @current_class_idx = -1
      i = i + 1
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
    # Add implicit return (last expression)
    last_type = infer_type(stmts.last)
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

  # ---- Feature detection ----
  def detect_features
    # Set up a temporary scope with main-level locals so feature detection
    # can infer types of local variables correctly
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
    if t == "RegularExpressionNode"
      @needs_regexp = 1
      # Collect pattern and flags
      pat = @nd_unescaped[nid]
      flags = "0"
      if @nd_flags[nid] != 0
        f = @nd_flags[nid]
        parts = "".split(",")
        # Prism flag bits → engine `RE_FLAG_*` values (see re_internal.h).
        # Prism: IGNORE_CASE=4, EXTENDED=8, MULTI_LINE=16.
        # Engine: IGNORECASE=1, MULTILINE=2, DOTALL=4, EXTENDED=8.
        # Ruby's /m (dot-matches-newline) maps to MULTILINE|DOTALL = 6.
        if f & 4 != 0
          parts.push("1")
        end
        if f & 16 != 0
          parts.push("6")
        end
        if f & 8 != 0
          parts.push("8")
        end
        if parts.length > 0
          flags = parts.join("|")
        end
      end
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

    if t == "GlobalVariableWriteNode"
      gname = @nd_name[nid]
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
      gname = @nd_name[nid]
      if gname != "$stderr" && gname != "$stdout" && gname != "$?"
        if not_in(gname, @gvar_names) == 1
          @gvar_names.push(gname)
          @gvar_types.push("int")
        end
      end
    end
    if t == "CallNode"
      mname = @nd_name[nid]
      # String methods that always need string helpers
      if mname == "to_s" || mname == "upcase" || mname == "downcase" ||
         mname == "strip" || mname == "chomp" || mname == "slice" ||
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
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_sym_intern = 1
          end
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

  def scan_features_children(nid)
    if @nd_body[nid] >= 0
      scan_features(@nd_body[nid])
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_features(stmts[k])
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_features(@nd_expression[nid])
    end
    if @nd_predicate[nid] >= 0
      scan_features(@nd_predicate[nid])
    end
    if @nd_subsequent[nid] >= 0
      scan_features(@nd_subsequent[nid])
    end
    if @nd_else_clause[nid] >= 0
      scan_features(@nd_else_clause[nid])
    end
    if @nd_receiver[nid] >= 0
      scan_features(@nd_receiver[nid])
    end
    if @nd_arguments[nid] >= 0
      scan_features(@nd_arguments[nid])
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_features(args[k])
      k = k + 1
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      scan_features(conds[k])
      k = k + 1
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_features(elems[k])
      k = k + 1
    end
    parts = parse_id_list(@nd_parts[nid])
    k = 0
    while k < parts.length
      scan_features(parts[k])
      k = k + 1
    end
    if @nd_left[nid] >= 0
      scan_features(@nd_left[nid])
    end
    if @nd_right[nid] >= 0
      scan_features(@nd_right[nid])
    end
    if @nd_block[nid] >= 0
      scan_features(@nd_block[nid])
    end
    if @nd_key[nid] >= 0
      scan_features(@nd_key[nid])
    end
    if @nd_collection[nid] >= 0
      scan_features(@nd_collection[nid])
    end
    if @nd_target[nid] >= 0
      scan_features(@nd_target[nid])
    end
    if @nd_parameters[nid] >= 0
      scan_features(@nd_parameters[nid])
    end
    reqs = parse_id_list(@nd_requireds[nid])
    k = 0
    while k < reqs.length
      scan_features(reqs[k])
      k = k + 1
    end
    opts = parse_id_list(@nd_optionals[nid])
    k = 0
    while k < opts.length
      scan_features(opts[k])
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
    all_bodies = @cls_meth_bodies[ci].split(";")
    all_mnames = @cls_meth_names[ci].split(";")
    all_params = @cls_meth_params[ci].split("|")
    all_ptypes = @cls_meth_ptypes[ci].split("|")
    bj = 0
    while bj < all_mnames.length
      if all_mnames[bj] == "initialize"
        bid = all_bodies[bj].to_i
        if bid >= 0
          pnames = "".split(",")
          ptypes = "".split(",")
          if bj < all_params.length
            pnames = all_params[bj].split(",")
          end
          if bj < all_ptypes.length
            ptypes = all_ptypes[bj].split(",")
          end
          # Find @ivar = param_name in initialize body
          resolve_ivar_from_body(ci, bid, iname, pnames, ptypes)
          ivt2 = cls_ivar_type(ci, iname)
          if ivt2 != "int"
            return ivt2
          end
        end
      end
      bj = bj + 1
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
            k = 0
            while k < arg_ids.length
              at = infer_type_deep(arg_ids[k])
              if k < ptypes.length
                ct = ptypes[k]
                # Skip rest/splat params (int_array) - they handle multiple args
                if ct == "int_array"
                  k = k + 1
                  next
                end
                # Issue #58: an empty `[]` literal at the call site is
                # compatible with any concrete typed-array param type.
                # Without this, `foo([])` against a body-promoted
                # `str_array` param triggers the ct != at mismatch and
                # bumps the param back to poly.
                if is_empty_array_literal(arg_ids[k]) == 1
                  if ct == "str_array" || ct == "float_array" || ct == "sym_array" || is_ptr_array_type(ct) == 1
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
    while pass < 5
      ci = 0
      while ci < @cls_names.length
        mnames = @cls_meth_names[ci].split(";")
        all_params = @cls_meth_params[ci].split("|")
        all_ptypes = @cls_meth_ptypes[ci].split("|")
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
            pnames_arr = "".split(",")
            if mi < all_params.length
              pnames_arr = all_params[mi].split(",")
            end
            ptypes_arr = "".split(",")
            if mi < all_ptypes.length
              ptypes_arr = all_ptypes[mi].split(",")
            end
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
        ci = ci + 1
      end
      pass = pass + 1
    end
  end

  def scan_cls_method_calls(ci, nid)
    if nid < 0
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
            all_ptypes = @cls_meth_ptypes[ci].split("|")
            if midx < all_ptypes.length
              ptypes = all_ptypes[midx].split(",")
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
              all_ptypes[midx] = ptypes.join(",")
              @cls_meth_ptypes[ci] = all_ptypes.join("|")
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

  def compile
    collect_all
    infer_main_call_types
    infer_function_body_call_types
    infer_class_body_call_types
    detect_poly_locals
    # Iterative type inference: converge param types, return types, ivar types.
    # Stop early when the signature of these three arrays stops changing.
    iter = 0
    prev_sig = inference_signature
    while iter < 4
      infer_all_returns
      infer_ivar_types_from_writers
      # Issue #58: after scan_locals has populated @meth_param_empty
      # via the per-call-site forward propagation, promote int_array
      # params to concrete typed-arrays where bodies push known types.
      # Then the next iteration's scan_locals back-propagates those
      # promoted types to caller-side locals.
      infer_param_array_type_from_body
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
    # Fix lambda return types based on call-site usage
    fix_lambda_return_types
    # Pre-detect bigint variables before feature detection
    pre_detect_bigint
    detect_features
    generate_code
  end

  def generate_code
    stmts = get_body_stmts(@root_id)

    detect_value_types
    recalc_needs_gc
    emit_raw("/* Generated by Spinel AOT compiler */")
    emit_raw("#include \"sp_runtime.h\"")
    # Emit Symbol intern table (Phase 2 Step 1: infrastructure only).
    collect_sym_names
    emit_sym_runtime
    # Emit program-specific regexp patterns
    if @needs_regexp == 1
      emit_regexp_runtime
    end
    emit_class_structs
    emit_raw("/*TUPLE_INSERT_POINT*/")
    emit_gc_scan_functions
    # Emit global variable declarations before functions
    gi = 0
    while gi < @gvar_names.length
      gt = @gvar_types[gi]
      cname = sanitize_gvar(@gvar_names[gi])
      ct = c_type(gt)
      if gt == "string"
        emit_raw("static " + ct + " " + cname + " = \"\";")
      elsif gt == "float"
        emit_raw("static " + ct + " " + cname + " = 0.0;")
      elsif type_is_pointer(gt) == 1
        emit_raw("static " + ct + " " + cname + " = NULL;")
      else
        emit_raw("static " + ct + " " + cname + " = 0;")
      end
      gi = gi + 1
    end
    emit_forward_decls
    emit_global_constants
    emit_raw("/*LAMBDA_INSERT_POINT*/")
    emit_class_methods
    emit_toplevel_methods
    # Emit lambda functions before main (they are generated during compilation)
    # We emit them in emit_main after forward declarations
    emit_main
    # Build tuple struct definitions into @deferred_tuple
    if @tuple_types.length > 0
      k = 0
      while k < @tuple_types.length
        t = @tuple_types[k]
        name = tuple_c_name(t)
        parts = tuple_elem_types_str(t).split(",")
        fields = ""
        fi = 0
        while fi < parts.length
          if fi > 0
            fields = fields + " "
          end
          fields = fields + c_type(parts[fi]) + " _" + fi.to_s + ";"
          fi = fi + 1
        end
        @deferred_tuple << "typedef struct { "
        @deferred_tuple << fields
        @deferred_tuple << " } "
        @deferred_tuple << name
        @deferred_tuple << ";\n"
        # GC scan function — only emit when at least one field is a GC ref.
        # Without this the tuple's children are collected while the tuple
        # itself is still alive, leaving dangling pointers in the fields.
        needs_scan = 0
        fi = 0
        while fi < parts.length
          if tuple_field_needs_mark(parts[fi]) == 1
            needs_scan = 1
          end
          fi = fi + 1
        end
        if needs_scan == 1
          body = ""
          fi = 0
          while fi < parts.length
            if tuple_field_needs_mark(parts[fi]) == 1
              field = "_t->_" + fi.to_s
              if parts[fi] == "poly"
                body = body + " sp_mark_rbval(" + field + ");"
              else
                body = body + " sp_gc_mark((void *)" + field + ");"
              end
            end
            fi = fi + 1
          end
          @deferred_tuple << "static void " + name + "_scan(void *_p) { " + name + " *_t = (" + name + " *)_p;" + body + " }\n"
        end
        k = k + 1
      end
    end
    0
  end

  def emit_header
    emit_raw("/* Generated by Spinel v2 AOT compiler */")
    emit_raw("#include <stdio.h>")
    emit_raw("#include <stdlib.h>")
    emit_raw("#include <string.h>")
    emit_raw("#include <math.h>")
    emit_raw("#include <stdbool.h>")
    emit_raw("#include <stdint.h>")
    emit_raw("#include <ctype.h>")
    emit_raw("#include <stdarg.h>")
    emit_raw("#include <time.h>")
    emit_raw("")
    emit_raw("typedef int64_t mrb_int;")
    emit_raw("typedef double mrb_float;")
    emit_raw("typedef bool mrb_bool;")
    emit_raw("#ifndef TRUE")
    emit_raw("#define TRUE true")
    emit_raw("#endif")
    emit_raw("#ifndef FALSE")
    emit_raw("#define FALSE false")
    emit_raw("#endif")
    emit_raw("")
    emit_raw("static inline mrb_int sp_idiv(mrb_int a, mrb_int b) {")
    emit_raw("  mrb_int q = a / b; mrb_int r = a % b;")
    emit_raw("  if ((r != 0) && ((r ^ b) < 0)) q--;")
    emit_raw("  return q;")
    emit_raw("}")
    emit_raw("static inline mrb_int sp_imod(mrb_int a, mrb_int b) {")
    emit_raw("  mrb_int r = a % b;")
    emit_raw("  if ((r != 0) && ((r ^ b) < 0)) r += b;")
    emit_raw("  return r;")
    emit_raw("}")
    emit_raw("")
    emit_raw("static mrb_int sp_gcd(mrb_int a,mrb_int b){if(a<0)a=-a;if(b<0)b=-b;while(b){mrb_int t=b;b=a%b;a=t;}return a;}")
    emit_raw("static mrb_int sp_int_clamp(mrb_int v,mrb_int lo,mrb_int hi){return v<lo?lo:v>hi?hi:v;}")
    emit_raw("static const char*sp_int_chr(mrb_int n){char*s=(char*)malloc(2);s[0]=(char)n;s[1]=0;return s;}")
    emit_raw("typedef struct{mrb_int first;mrb_int last;}sp_Range;")
    emit_raw("static sp_Range sp_range_new(mrb_int f,mrb_int l){sp_Range r;r.first=f;r.last=l;return r;}")
    if @needs_system == 1
      emit_raw("static int sp_last_status = 0;")
    end
    emit_raw("")
  end

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
    # Promote all int variables in the same scope that interact with bigint
    if @needs_bigint == 1
      scan_bigint_propagate(stmts, names, types)
    end
  end

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

  def scan_bigint_propagate(stmts, names, types)
    # Propagate: if x is bigint and y = expr involving x, y becomes bigint
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
    # StatementsNode children
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
  def emit_sym_int_hash_runtime
    emit_raw("typedef struct{sp_sym*keys;mrb_int*vals;sp_sym*order;mrb_int len;mrb_int cap;mrb_int mask;}sp_SymIntHash;")
    emit_raw("static void sp_SymIntHash_fin(void*p){sp_SymIntHash*h=(sp_SymIntHash*)p;free(h->keys);free(h->vals);free(h->order);}")
    emit_raw("static sp_SymIntHash*sp_SymIntHash_new(void){sp_SymIntHash*h=(sp_SymIntHash*)sp_gc_alloc(sizeof(sp_SymIntHash),sp_SymIntHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);h->len=0;return h;}")
    emit_raw("static void sp_SymIntHash_grow(sp_SymIntHash*h){mrb_int oc=h->cap;sp_sym*ok=h->keys;mrb_int*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(sp_sym*)realloc(h->order,sizeof(sp_sym)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]>=0){mrb_int idx=(mrb_int)(((mrb_int)ok[i])&h->mask);while(h->keys[idx]>=0)idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}")
    emit_raw("static mrb_int sp_SymIntHash_get(sp_SymIntHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return 0;}")
    emit_raw("static void sp_SymIntHash_set(sp_SymIntHash*h,sp_sym k,mrb_int v){if(h->len*2>=h->cap)sp_SymIntHash_grow(h);mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}")
    emit_raw("static mrb_bool sp_SymIntHash_has_key(sp_SymIntHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}")
    emit_raw("static mrb_int sp_SymIntHash_length(sp_SymIntHash*h){return h->len;}")
    emit_raw("static void sp_SymIntHash_delete(sp_SymIntHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->keys[idx]=-1;h->vals[idx]=0;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]>=0){mrb_int nj=(mrb_int)(((mrb_int)h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=-1;h->vals[j]=0;idx=j;}j=(j+1)&h->mask;}return;}idx=(idx+1)&h->mask;}}")
    emit_raw("")
  end

  # Symbol-keyed hash with string values.
  def emit_sym_str_hash_runtime
    emit_raw("typedef struct{sp_sym*keys;const char**vals;sp_sym*order;mrb_int len;mrb_int cap;mrb_int mask;}sp_SymStrHash;")
    emit_raw("static void sp_SymStrHash_fin(void*p){sp_SymStrHash*h=(sp_SymStrHash*)p;free(h->keys);free(h->vals);free(h->order);}")
    emit_raw("static sp_SymStrHash*sp_SymStrHash_new(void){sp_SymStrHash*h=(sp_SymStrHash*)sp_gc_alloc(sizeof(sp_SymStrHash),sp_SymStrHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);h->len=0;return h;}")
    emit_raw("static void sp_SymStrHash_grow(sp_SymStrHash*h){mrb_int oc=h->cap;sp_sym*ok=h->keys;const char**ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(sp_sym*)realloc(h->order,sizeof(sp_sym)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]>=0){mrb_int idx=(mrb_int)(((mrb_int)ok[i])&h->mask);while(h->keys[idx]>=0)idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}")
    emit_raw("static const char*sp_SymStrHash_get(sp_SymStrHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return\"\";}")
    emit_raw("static void sp_SymStrHash_set(sp_SymStrHash*h,sp_sym k,const char*v){if(h->len*2>=h->cap)sp_SymStrHash_grow(h);mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}")
    emit_raw("static mrb_bool sp_SymStrHash_has_key(sp_SymStrHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}")
    emit_raw("static mrb_int sp_SymStrHash_length(sp_SymStrHash*h){return h->len;}")
    emit_raw("static void sp_SymStrHash_delete(sp_SymStrHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->keys[idx]=-1;h->vals[idx]=NULL;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]>=0){mrb_int nj=(mrb_int)(((mrb_int)h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=-1;h->vals[j]=NULL;idx=j;}j=(j+1)&h->mask;}return;}idx=(idx+1)&h->mask;}}")
    emit_raw("")
  end

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
  def emit_sym_runtime
    if @sym_names.length > 0
      emit_raw("/* sp_sym intern table */")
      emit_raw("#define SP_SYM_COUNT " + @sym_names.length.to_s)
      line = "static const char *const sp_sym_names[" + @sym_names.length.to_s + "] = {"
      i = 0
      while i < @sym_names.length
        if i > 0
          line = line + ","
        end
        line = line + c_string_literal(@sym_names[i])
        i = i + 1
      end
      line = line + "};"
      emit_raw(line)
      if @needs_sym_intern == 1
        # Dynamic intern pool: String#to_sym at runtime.
        emit_raw("static const char **sp_sym_dyn_names = NULL;")
        emit_raw("static mrb_int sp_sym_dyn_count = 0;")
        emit_raw("static mrb_int sp_sym_dyn_cap = 0;")
        emit_raw("static sp_sym sp_sym_intern(const char *s) __attribute__((unused));")
        emit_raw("static sp_sym sp_sym_intern(const char *s){mrb_int i;for(i=0;i<SP_SYM_COUNT;i++){if(strcmp(sp_sym_names[i],s)==0)return (sp_sym)i;}for(i=0;i<sp_sym_dyn_count;i++){if(strcmp(sp_sym_dyn_names[i],s)==0)return (sp_sym)(SP_SYM_COUNT+i);}if(sp_sym_dyn_count>=sp_sym_dyn_cap){sp_sym_dyn_cap=sp_sym_dyn_cap?sp_sym_dyn_cap*2:8;sp_sym_dyn_names=(const char**)realloc(sp_sym_dyn_names,sizeof(char*)*sp_sym_dyn_cap);}{size_t sl=strlen(s);char*dup=(char*)malloc(sl+1);memcpy(dup,s,sl+1);sp_sym_dyn_names[sp_sym_dyn_count]=dup;}return (sp_sym)(SP_SYM_COUNT+sp_sym_dyn_count++);}")
        emit_raw("static const char *sp_sym_to_s(sp_sym id) __attribute__((unused));")
        emit_raw("static const char *sp_sym_to_s(sp_sym id){if(id<0)return \"\";if(id<SP_SYM_COUNT)return sp_sym_names[id];mrb_int idx=(mrb_int)id-SP_SYM_COUNT;if(idx>=sp_sym_dyn_count)return \"\";return sp_sym_dyn_names[idx];}")
      else
        emit_raw("static const char *sp_sym_to_s(sp_sym id) __attribute__((unused));")
        emit_raw("static const char *sp_sym_to_s(sp_sym id){if(id>=0&&id<SP_SYM_COUNT)return sp_sym_names[id];return \"\";}")
      end
      # Emit SPS_<name> defines for symbols that form valid C identifiers.
      i = 0
      while i < @sym_names.length
        nm = @sym_names[i]
        if sym_is_c_ident(nm) == 1
          emit_raw("#define SPS_" + nm + " ((sp_sym)" + i.to_s + ")")
        end
        i = i + 1
      end
      # Sort comparator for sym arrays: lexical by symbol name.
      emit_raw("static int sp_sym_sort_cmp(const void*a,const void*b) __attribute__((unused));")
      emit_raw("static int sp_sym_sort_cmp(const void*a,const void*b){return strcmp(sp_sym_to_s(*(const sp_sym*)a),sp_sym_to_s(*(const sp_sym*)b));}")
      emit_raw("static void sp_sym_array_sort(sp_IntArray*a) __attribute__((unused));")
      emit_raw("static void sp_sym_array_sort(sp_IntArray*a){qsort(a->data+a->start,a->len,sizeof(mrb_int),sp_sym_sort_cmp);}")
    else
      # No symbols used at all — provide stub for sp_runtime.h's SP_TAG_SYM
      emit_raw("static const char *sp_sym_to_s(sp_sym id){(void)id;return \"\";}")
    end
    emit_raw("")
    if @needs_sym_int_hash == 1
      emit_sym_int_hash_runtime
    end
    if @needs_sym_str_hash == 1
      emit_sym_str_hash_runtime
    end
  end

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
  def compile_expr_as_string(nid)
    s = compile_expr(nid)
    if infer_type(nid) == "symbol"
      return "sp_sym_to_s(" + s + ")"
    end
    s
  end

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

  def emit_regexp_runtime
    # Common regexp types and helpers are in sp_runtime.h
    # Common regexp types and helpers are in sp_runtime.h
    # Only emit program-specific pattern globals and init here
    # Emit compiled pattern globals
    i = 0
    while i < @regexp_patterns.length
      pat = @regexp_patterns[i]
      flags = @regexp_flags[i]
      emit_raw("static mrb_regexp_pattern *sp_re_pat_" + i.to_s + ";")
      i = i + 1
    end
    emit_raw("")
    emit_raw("static void sp_re_init(void) {")
    i = 0
    while i < @regexp_patterns.length
      pat = @regexp_patterns[i]
      flags = @regexp_flags[i]
      cpat = ""
      pi = 0
      while pi < pat.length
        ch = pat[pi]
        if ch == 92.chr
          cpat = cpat + 92.chr + 92.chr
        elsif ch == 34.chr
          cpat = cpat + 92.chr + 34.chr
        elsif ch == 10.chr
          # Embedded newlines / tabs / CRs in /x patterns must be
          # encoded as escape sequences; emitting them raw produces an
          # unterminated C string literal.
          cpat = cpat + 92.chr + "n"
        elsif ch == 13.chr
          cpat = cpat + 92.chr + "r"
        elsif ch == 9.chr
          cpat = cpat + 92.chr + "t"
        else
          cpat = cpat + ch
        end
        pi = pi + 1
      end
      # Byte count, not character count — `re_compile` reads `pat`
       # as a byte buffer, and a UTF-8 char class (`[₀₁₂…]`) has
      # `pat.length < pat.bytesize`. Truncating to char count cuts
      # off a multi-byte char mid-sequence and the engine reports
      # "unterminated character class". Issue #61.
      emit_raw("  sp_re_pat_" + i.to_s + " = re_compile(\"" + cpat + "\", " + pat.bytesize.to_s + ", " + flags + ");")
      i = i + 1
    end
    emit_raw("}")
    emit_raw("")
  end

  # ---- Struct emission ----
  def emit_global_constants
    # Emit file-scope constant declarations (initialized in main)
    i = 0
    while i < @const_names.length
      ctp = c_type(@const_types[i])
      emit_raw("static " + ctp + " cst_" + @const_names[i] + " = " + c_default_val(@const_types[i]) + ";")
      i = i + 1
    end
    if @const_names.length > 0
      emit_raw("")
    end
  end

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

  def self_arrow
    if @current_class_idx >= 0
      if @cls_is_value_type[@current_class_idx] == 1
        return "self."
      end
    end
    "self->"
  end

  def check_ivar_write_child(child_nid)
    if child_nid >= 0
      if subtree_has_ivar_write(child_nid) == 1
        return 1
      end
    end
    0
  end

  def check_ivar_write_list(list_str)
    if list_str != ""
      parts = list_str.split(",")
      pi = 0
      while pi < parts.length
        id = parts[pi].to_i
        if id > 0 && subtree_has_ivar_write(id) == 1
          return 1
        end
        pi = pi + 1
      end
    end
    0
  end

  def subtree_has_ivar_write(nid)
    if nid < 0 || nid >= @nd_count
      return 0
    end
    t = @nd_type[nid]
    if t == "InstanceVariableWriteNode" || t == "InstanceVariableOperatorWriteNode" || t == "InstanceVariableTargetNode"
      return 1
    end
    # Check integer child references
    if check_ivar_write_child(@nd_body[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_expression[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_predicate[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_subsequent[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_else_clause[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_left[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_right[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_target[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_rescue_clause[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_ensure_clause[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_collection[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_rest[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_arguments[nid]) == 1; return 1; end
    if check_ivar_write_child(@nd_block[nid]) == 1; return 1; end
    # Check string-list children (comma-separated node IDs)
    if check_ivar_write_list(@nd_stmts[nid]) == 1; return 1; end
    if check_ivar_write_list(@nd_elements[nid]) == 1; return 1; end
    if check_ivar_write_list(@nd_targets[nid]) == 1; return 1; end
    if check_ivar_write_list(@nd_rights[nid]) == 1; return 1; end
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

  def check_setter_on_params_child(child_nid, param_names)
    if child_nid >= 0
      r = subtree_has_setter_on_params(child_nid, param_names)
      if r != ""
        return r
      end
    end
    ""
  end

  def check_setter_on_params_list(list_str, param_names)
    if list_str != ""
      parts = list_str.split(",")
      pi2 = 0
      while pi2 < parts.length
        id = parts[pi2].to_i
        if id > 0
          r = subtree_has_setter_on_params(id, param_names)
          if r != ""
            return r
          end
        end
        pi2 = pi2 + 1
      end
    end
    ""
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
    # Recurse into integer children
    r = check_setter_on_params_child(@nd_body[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_expression[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_predicate[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_subsequent[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_else_clause[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_left[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_right[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_target[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_rescue_clause[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_ensure_clause[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_collection[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_rest[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_arguments[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_child(@nd_block[nid], param_names)
    if r != ""; return r; end
    # Recurse into string-list children
    r = check_setter_on_params_list(@nd_stmts[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_list(@nd_elements[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_list(@nd_targets[nid], param_names)
    if r != ""; return r; end
    r = check_setter_on_params_list(@nd_rights[nid], param_names)
    if r != ""; return r; end
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
    # Recurse into the conventional child slots.
    if @nd_body[nid] >= 0
      collect_constructed_class_names(@nd_body[nid], out)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      collect_constructed_class_names(stmts[k], out)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      collect_constructed_class_names(@nd_expression[nid], out)
    end
    if @nd_predicate[nid] >= 0
      collect_constructed_class_names(@nd_predicate[nid], out)
    end
    if @nd_subsequent[nid] >= 0
      collect_constructed_class_names(@nd_subsequent[nid], out)
    end
    if @nd_else_clause[nid] >= 0
      collect_constructed_class_names(@nd_else_clause[nid], out)
    end
    if @nd_receiver[nid] >= 0
      collect_constructed_class_names(@nd_receiver[nid], out)
    end
    if @nd_arguments[nid] >= 0
      collect_constructed_class_names(@nd_arguments[nid], out)
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      collect_constructed_class_names(args[k], out)
      k = k + 1
    end
    if @nd_left[nid] >= 0
      collect_constructed_class_names(@nd_left[nid], out)
    end
    if @nd_right[nid] >= 0
      collect_constructed_class_names(@nd_right[nid], out)
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
    # well-typed. Mirrors the ptr_array exclusion (PR #87).
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
          # void* mismatch as the ptr_array case (issue #118).
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
      if eligible == 1
        @cls_is_sra[i] = 1
      end
      i = i + 1
    end
  end

  # Return "static inline " for short methods so gcc has permission
  # to inline them, or "static " otherwise.  Body of ≤ 3 statements,
  # no yield, and not self-recursive are considered inlineable.
  def method_linkage(body_id, has_yield)
    method_linkage_named(body_id, has_yield, "")
  end

  def method_linkage_named(body_id, has_yield, mname)
    if has_yield == 1
      return "static "
    end
    if body_id < 0
      return "static inline "
    end
    stmts = get_stmts(body_id)
    if stmts.length > 3
      return "static "
    end
    # Avoid inlining self-recursive methods: static inline on a recursive
    # function can blow up code size and hurt performance (gcc tries
    # to inline harder than it should).
    if mname != "" && node_calls_name?(body_id, mname) == 1
      return "static "
    end
    "static inline "
  end

  # Return 1 if any CallNode in the subtree invokes mname.
  def node_calls_name?(nid, mname)
    if nid < 0
      return 0
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == mname
      return 1
    end
    if @nd_receiver[nid] >= 0
      if node_calls_name?(@nd_receiver[nid], mname) == 1
        return 1
      end
    end
    args_id = @nd_arguments[nid]
    if args_id >= 0
      arr = get_args(args_id)
      k = 0
      while k < arr.length
        if node_calls_name?(arr[k], mname) == 1
          return 1
        end
        k = k + 1
      end
    end
    if @nd_body[nid] >= 0
      if node_calls_name?(@nd_body[nid], mname) == 1
        return 1
      end
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      if node_calls_name?(stmts[k], mname) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_subsequent[nid] >= 0
      if node_calls_name?(@nd_subsequent[nid], mname) == 1
        return 1
      end
    end
    0
  end

  def emit_tuple_structs
    # Tuple structs are now inserted at end of generate_code
  end

  def emit_class_structs
    # Forward declare typedefs
    i = 0
    while i < @cls_names.length
      emit_raw("typedef struct sp_" + @cls_names[i] + "_s sp_" + @cls_names[i] + ";")
      i = i + 1
    end
    if @cls_names.length > 0
      emit_raw("")
    end
    # Struct definitions, ordered so a class with a value-type ivar of
    # type X is emitted after X (C requires the embedded struct's full
    # definition to be visible). Pointer-typed ivars only need the
    # forward typedef above, so they impose no ordering constraint.
    @struct_emitted = []
    i = 0
    while i < @cls_names.length
      @struct_emitted.push(0)
      i = i + 1
    end
    i = 0
    while i < @cls_names.length
      emit_class_struct_with_deps(i)
      i = i + 1
    end
  end

  def emit_class_struct_with_deps(ci)
    if @struct_emitted[ci] == 1
      return
    end
    @struct_emitted[ci] = 1
    # Walk this class and all its ancestors (whose ivars are flattened
    # into this struct by emit_parent_fields) and emit any value-type
    # field's class struct first.
    ai = ci
    while ai >= 0
      emit_value_type_field_deps(ci, ai)
      if @cls_parents[ai] != ""
        ai = find_class_idx(@cls_parents[ai])
      else
        ai = -1
      end
    end
    emit_raw("struct sp_" + @cls_names[ci] + "_s {")
    emit_class_fields(ci)
    emit_raw("};")
    emit_raw("")
  end

  def emit_value_type_field_deps(orig_ci, ai)
    types = @cls_ivar_types[ai].split(";")
    j = 0
    while j < types.length
      emit_value_type_field_dep(orig_ci, types[j])
      j = j + 1
    end
  end

  def emit_value_type_field_dep(orig_ci, t)
    if is_value_type_obj(t) == 1
      cname = t[4, t.length - 4]
      di = find_class_idx(cname)
      if di >= 0 && di != orig_ci
        emit_class_struct_with_deps(di)
      end
    end
  end

  def emit_class_fields(ci)
    # Parent fields first
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        emit_parent_fields(pi)
      end
    end
    # Own fields (skip those inherited from parent)
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    j = 0
    while j < names.length
      iname = names[j]
      itype = "int"
      if j < types.length
        itype = types[j]
      end
      # Skip if in parent chain
      if @cls_parents[ci] != ""
        pi = find_class_idx(@cls_parents[ci])
        if pi >= 0
          if ivar_in_chain(pi, iname) == 1
            j = j + 1
            next
          end
        end
      end
      fname = sanitize_ivar(iname)
      emit_raw("  " + c_type(itype) + " " + fname + ";")
      j = j + 1
    end
  end

  def emit_parent_fields(ci)
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        emit_parent_fields(pi)
      end
    end
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    j = 0
    while j < names.length
      iname = names[j]
      itype = "int"
      if j < types.length
        itype = types[j]
      end
      if @cls_parents[ci] != ""
        pi = find_class_idx(@cls_parents[ci])
        if pi >= 0
          if ivar_in_chain(pi, iname) == 1
            j = j + 1
            next
          end
        end
      end
      fname = sanitize_ivar(iname)
      emit_raw("  " + c_type(itype) + " " + fname + ";")
      j = j + 1
    end
  end

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

  def ivar_is_gc_ptr(t)
    if is_obj_type(t) == 1
      if is_value_type_obj(t) == 1
        return 0
      end
      return 1
    end
    if type_is_pointer(t) == 1
      return 1
    end
    0
  end

  def class_has_ptr_ivars(ci)
    names = @cls_ivar_names[ci].split(";")
    types = @cls_ivar_types[ci].split(";")
    j = 0
    while j < names.length
      if j < types.length
        if ivar_is_gc_ptr(types[j]) == 1
          return 1
        end
      end
      j = j + 1
    end
    if @cls_parents[ci] != ""
      pi = find_class_idx(@cls_parents[ci])
      if pi >= 0
        return class_has_ptr_ivars(pi)
      end
    end
    0
  end

  def emit_gc_scan_functions
    i = 0
    while i < @cls_names.length
      if class_has_ptr_ivars(i) == 1
        cname = @cls_names[i]
        emit_raw("static void sp_" + cname + "_gc_scan(void *p) {")
        emit_raw("  sp_" + cname + " *self = (sp_" + cname + " *)p;")
        names = @cls_ivar_names[i].split(";")
        types = @cls_ivar_types[i].split(";")
        j = 0
        while j < names.length
          if j < types.length
            if ivar_is_gc_ptr(types[j]) == 1
              emit_raw("  if (self->" + sanitize_ivar(names[j]) + ") sp_gc_mark((void *)self->" + sanitize_ivar(names[j]) + ");")
            end
          end
          j = j + 1
        end
        # Also scan parent fields
        if @cls_parents[i] != ""
          pi = find_class_idx(@cls_parents[i])
          if pi >= 0
            pnames = @cls_ivar_names[pi].split(";")
            ptypes = @cls_ivar_types[pi].split(";")
            pj = 0
            while pj < pnames.length
              if pj < ptypes.length
                if ivar_is_gc_ptr(ptypes[pj]) == 1
                  emit_raw("  if (self->" + sanitize_ivar(pnames[pj]) + ") sp_gc_mark((void *)self->" + sanitize_ivar(pnames[pj]) + ");")
                end
              end
              pj = pj + 1
            end
          end
        end
        emit_raw("}")
        emit_raw("")
      end
      i = i + 1
    end
  end

  # ---- Forward declarations ----
  def emit_forward_decls
    # Emit block helper functions accumulated during collection
    if @block_funcs != ""
      emit_raw(@block_funcs)
    end
    # Top-level methods
    i = 0
    while i < @meth_names.length
      yp = ""
      if @meth_has_yield[i] == 1
        yp = yield_params_suffix(i)
      end
      emit_raw(method_linkage_named(@meth_body_ids[i], @meth_has_yield[i], @meth_names[i]) + c_type(@meth_return_types[i]) + " sp_" + sanitize_name(@meth_names[i]) + "(" + method_params_decl(i) + yp + ");")
      i = i + 1
    end
    # Class methods
    i = 0
    while i < @cls_names.length
      cname = @cls_names[i]
      # Constructor
      init_idx = cls_find_method_direct(i, "initialize")
      if @cls_is_value_type[i] == 1
        emit_raw("static sp_" + cname + " sp_" + cname + "_new(" + constructor_params_decl(i) + ");")
      else
        emit_raw("static sp_" + cname + " *sp_" + cname + "_new(" + constructor_params_decl(i) + ");")
      end
      if init_idx >= 0
        emit_raw("static inline void sp_" + cname + "_initialize(sp_" + cname + " *self" + init_params_decl(i) + ");")
      end
      # Instance methods
      mnames = @cls_meth_names[i].split(";")
      returns = @cls_meth_returns[i].split(";")
      all_params = @cls_meth_params[i].split("|")
      all_ptypes = @cls_meth_ptypes[i].split("|")
      j = 0
      while j < mnames.length
        if mnames[j] != "initialize"
          rt = "int"
          if j < returns.length
            rt = returns[j]
          end
          yp = ""
          if cls_method_has_yield(i, j) == 1
            yp = yield_params_suffix_cls(i, j)
          end
          sp = " *self"
          if @cls_is_value_type[i] == 1
            sp = " self"
          end
          bids = @cls_meth_bodies[i].split(";")
          bid_j = j < bids.length ? bids[j].to_i : -1
          emit_raw(method_linkage_named(bid_j, cls_method_has_yield(i, j), mnames[j]) + c_type(rt) + " sp_" + cname + "_" + sanitize_name(mnames[j]) + "(sp_" + cname + sp + method_with_self_params(j, all_params, all_ptypes) + yp + ");")
        end
        j = j + 1
      end
      # Class methods
      cmnames = @cls_cmeth_names[i].split(";")
      cm_returns = @cls_cmeth_returns[i].split(";")
      cm_params = @cls_cmeth_params[i].split("|")
      cm_ptypes = @cls_cmeth_ptypes[i].split("|")
      j = 0
      while j < cmnames.length
        rt = "int"
        if j < cm_returns.length
          rt = cm_returns[j]
        end
        emit_raw("static " + c_type(rt) + " sp_" + cname + "_cls_" + sanitize_name(cmnames[j]) + "(" + cls_method_params_decl(j, cm_params, cm_ptypes) + ");")
        j = j + 1
      end
      i = i + 1
    end
    emit_raw("")
  end

  def yield_params_suffix(mi)
    pd = method_params_decl(mi)
    if pd == ""
      return "void (*_block)(mrb_int, void*), void *_benv"
    end
    return ", void (*_block)(mrb_int, void*), void *_benv"
  end

  def yield_params_suffix_cls(ci, midx)
    # For class instance methods (always have self first)
    return ", void (*_block)(mrb_int, void*), void *_benv"
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

  # Returns 1 if the (ci, midx) method declares a `&block` parameter,
  # 0 otherwise. Ruby syntax requires `&block` to be the trailing
  # param, so we check only the last slot — a proc-typed slot in any
  # other position is a positional proc argument, not a block param.
  # Mirrors cls_method_has_yield: call sites use it to decide whether
  # to omit the trailing &block slot from default-padding.
  def cls_method_has_block_param(ci, midx)
    if ci < 0 || midx < 0
      return 0
    end
    all_ptypes = @cls_meth_ptypes[ci].split("|")
    if midx >= all_ptypes.length
      return 0
    end
    pts = all_ptypes[midx].split(",")
    (pts.length > 0 && pts.last == "proc") ? 1 : 0
  end

  # Returns the name of a method's `&block` parameter (the trailing
  # proc-typed slot in pnames), or "" if the method doesn't take
  # one. Ruby syntax requires `&block` to be the trailing param, so
  # a proc-typed slot in any other position is a positional proc
  # argument. Mirrors cls_method_has_block_param's trailing-only
  # check. Used at method-emit time to set @current_method_block_param
  # so block_given? can resolve to (lv_<name> != NULL).
  def find_block_param_name(pnames, ptypes)
    if ptypes.length > 0 && ptypes.last == "proc"
      return pnames.last
    end
    ""
  end

  def cls_find_method_direct(ci, mname)
    mnames = @cls_meth_names[ci].split(";")
    j = 0
    while j < mnames.length
      if mnames[j] == mname
        return j
      end
      j = j + 1
    end
    -1
  end

  def method_params_decl(mi)
    mfullname = @meth_names[mi]
    pnames = @meth_param_names[mi].split(",")
    ptypes = @meth_param_types[mi].split(",")
    # Check for open class method
    oc_self = ""
    if mfullname.start_with?("__oc_Integer_")
      oc_self = "mrb_int self"
    end
    if mfullname.start_with?("__oc_String_")
      oc_self = "const char * self"
    end
    if mfullname.start_with?("__oc_Float_")
      oc_self = "mrb_float self"
    end
    if oc_self != ""
      if pnames.length == 0
        return oc_self
      end
      result = oc_self
      j = 0
      while j < pnames.length
        result = result + ", "
        pt = "int"
        if j < ptypes.length
          pt = ptypes[j]
        end
        result = result + c_type(pt) + " lv_" + pnames[j]
        j = j + 1
      end
      return result
    end
    if pnames.length == 0
      if @meth_has_yield[mi] == 1
        return ""
      end
      return "void"
    end
    result = ""
    j = 0
    while j < pnames.length
      if j > 0
        result = result + ", "
      end
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

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

  def constructor_params_decl(ci)
    init_ci = find_init_class(ci)
    if init_ci < 0
      return "void"
    end
    init_idx = cls_find_method_direct(init_ci, "initialize")
    all_params = @cls_meth_params[init_ci].split("|")
    all_ptypes = @cls_meth_ptypes[init_ci].split("|")
    pnames = "".split(",")
    ptypes = "".split(",")

    if init_idx < all_params.length
      pnames = all_params[init_idx].split(",")
    end
    if init_idx < all_ptypes.length
      ptypes = all_ptypes[init_idx].split(",")
    end
    if pnames.length == 0
      return "void"
    end
    result = ""
    j = 0
    while j < pnames.length
      if j > 0
        result = result + ", "
      end
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

  def init_params_decl(ci)
    init_ci = find_init_class(ci)
    if init_ci < 0
      return ""
    end
    init_idx = cls_find_method_direct(init_ci, "initialize")
    if init_idx < 0
      return ""
    end
    all_params = @cls_meth_params[init_ci].split("|")
    all_ptypes = @cls_meth_ptypes[init_ci].split("|")
    pnames = "".split(",")
    ptypes = "".split(",")

    if init_idx < all_params.length
      pnames = all_params[init_idx].split(",")
    end
    if init_idx < all_ptypes.length
      ptypes = all_ptypes[init_idx].split(",")
    end
    result = ""
    j = 0
    while j < pnames.length
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + ", " + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

  def method_with_self_params(midx, all_params, all_ptypes)
    pnames = "".split(",")
    ptypes = "".split(",")

    if midx < all_params.length
      pnames = all_params[midx].split(",")
    end
    if midx < all_ptypes.length
      ptypes = all_ptypes[midx].split(",")
    end
    result = ""
    j = 0
    while j < pnames.length
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + ", " + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

  def cls_method_params_decl(midx, all_params, all_ptypes)
    pnames = "".split(",")
    ptypes = "".split(",")

    if midx < all_params.length
      pnames = all_params[midx].split(",")
    end
    if midx < all_ptypes.length
      ptypes = all_ptypes[midx].split(",")
    end
    if pnames.length == 0
      return "void"
    end
    result = ""
    j = 0
    while j < pnames.length
      if j > 0
        result = result + ", "
      end
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

  # ---- Emit class methods ----
  def emit_class_methods
    i = 0
    while i < @cls_names.length
      emit_constructor(i)
      mnames = @cls_meth_names[i].split(";")
      returns = @cls_meth_returns[i].split(";")
      all_params = @cls_meth_params[i].split("|")
      all_ptypes = @cls_meth_ptypes[i].split("|")
      bodies = @cls_meth_bodies[i].split(";")
      j = 0
      while j < mnames.length
        if mnames[j] != "initialize"
          rt = "int"
          if j < returns.length
            rt = returns[j]
          end
          bid = -1
          if j < bodies.length
            bid = bodies[j].to_i
          end
          pnames = "".split(",")
          ptypes = "".split(",")
          if j < all_params.length
            pnames = all_params[j].split(",")
          end
          if j < all_ptypes.length
            ptypes = all_ptypes[j].split(",")
          end
          emit_instance_method(i, mnames[j], pnames, ptypes, rt, bid)
        end
        j = j + 1
      end
      # Class methods
      cmnames = @cls_cmeth_names[i].split(";")
      cm_returns = @cls_cmeth_returns[i].split(";")
      cm_params = @cls_cmeth_params[i].split("|")
      cm_ptypes = @cls_cmeth_ptypes[i].split("|")
      cm_bodies = @cls_cmeth_bodies[i].split(";")
      j = 0
      while j < cmnames.length
        rt = "int"
        if j < cm_returns.length
          rt = cm_returns[j]
        end
        bid = -1
        if j < cm_bodies.length
          bid = cm_bodies[j].to_i
        end
        pnames = "".split(",")
        ptypes = "".split(",")

        if j < cm_params.length
          pnames = cm_params[j].split(",")
        end
        if j < cm_ptypes.length
          ptypes = cm_ptypes[j].split(",")
        end
        emit_class_level_method(i, cmnames[j], pnames, ptypes, rt, bid)
        j = j + 1
      end
      i = i + 1
    end
  end

  def emit_constructor(ci)
    saved_gc_scope = @in_gc_scope
    cname = @cls_names[ci]
    init_idx = cls_find_method_direct(ci, "initialize")
    if @cls_is_value_type[ci] == 1
      emit_raw("static sp_" + cname + " sp_" + cname + "_new(" + constructor_params_decl(ci) + ") {")
      emit_raw("  sp_" + cname + " self = {0};")
    else
      emit_raw("static inline sp_" + cname + " *sp_" + cname + "_new(" + constructor_params_decl(ci) + ") {")
      emit_raw("  SP_GC_SAVE();")
      @in_gc_scope = 1
      scan_fn = "NULL"
      if class_has_ptr_ivars(ci) == 1
        scan_fn = "sp_" + cname + "_gc_scan"
      end
      emit_raw("  sp_" + cname + " *self = (sp_" + cname + " *)sp_gc_alloc(sizeof(sp_" + cname + "), NULL, " + scan_fn + ");")
      emit_raw("  SP_GC_ROOT(self);")
    end

    # Root pointer-type constructor parameters
    if init_idx >= 0
      all_params_str = @cls_meth_params[ci].split("|")
      all_ptypes_str = @cls_meth_ptypes[ci].split("|")
      if init_idx < all_params_str.length
        cp_names = all_params_str[init_idx].split(",")
        cp_types = "".split(",")
        if init_idx < all_ptypes_str.length
          cp_types = all_ptypes_str[init_idx].split(",")
        end
        cpi = 0
        while cpi < cp_names.length
          if cpi < cp_types.length
            if type_is_pointer(cp_types[cpi]) == 1
              emit_raw("  SP_GC_ROOT(lv_" + cp_names[cpi] + ");")
            end
          end
          cpi = cpi + 1
        end
      end
    end

    init_ci = find_init_class(ci)

    if init_idx >= 0
      bodies = @cls_meth_bodies[ci].split(";")
      bid = -1
      if init_idx < bodies.length
        bid = bodies[init_idx].to_i
      end
      if bid == -2
        # Synthetic struct constructor
        all_params = @cls_meth_params[ci].split("|")
        pnames2 = "".split(",")
        if init_idx < all_params.length
          pnames2 = all_params[init_idx].split(",")
        end
        sk = 0
        while sk < pnames2.length
          sa = "->"
          if @cls_is_value_type[ci] == 1
            sa = "."
          end
          emit_raw("  self" + sa + sanitize_ivar(pnames2[sk]) + " = lv_" + pnames2[sk] + ";")
          sk = sk + 1
        end
      end
      if bid >= 0
        @current_class_idx = ci
        all_params = @cls_meth_params[ci].split("|")
        all_ptypes = @cls_meth_ptypes[ci].split("|")
        pnames = "".split(",")
        ptypes = "".split(",")

        if init_idx < all_params.length
          pnames = all_params[init_idx].split(",")
        end
        if init_idx < all_ptypes.length
          ptypes = all_ptypes[init_idx].split(",")
        end
        push_scope
        k = 0
        while k < pnames.length
          pt = "int"
          if k < ptypes.length
            pt = ptypes[k]
          end
          declare_var(pnames[k], pt)
          k = k + 1
        end
        # Declare any local variables used inside initialize so that
        # `x = 1; @a = x` style bodies don't reference an undeclared lv_x.
        declare_method_locals(bid, pnames)
        stmts = get_stmts(bid)
        stmts.each { |sid|
          if @nd_type[sid] == "SuperNode"
            if @cls_parents[ci] != ""
              pi = find_class_idx(@cls_parents[ci])
              if pi >= 0
                super_args = ""
                args_id = @nd_arguments[sid]
                if args_id >= 0
                  arg_ids = get_args(args_id)
                  ak = 0
                  while ak < arg_ids.length
                    if ak > 0
                      super_args = super_args + ", "
                    end
                    super_args = super_args + compile_expr(arg_ids[ak])
                    ak = ak + 1
                  end
                end
                emit_raw("  sp_" + @cls_parents[ci] + "_initialize((sp_" + @cls_parents[ci] + " *)self" + (super_args != "" ? ", " + super_args : "") + ");")
              end
            end
          end
          if @nd_type[sid] == "InstanceVariableWriteNode"
            ivar_name = @nd_name[sid]
            ivar = sanitize_ivar(ivar_name)
            expr_id_iv = @nd_expression[sid]
            # Match the special-case in compile_stmt: an empty `{}`
            # assigned to an ivar promoted by scan_writer_calls needs
            # the matching `sp_*Hash_new()` constructor (issue #64).
            ivt = cls_ivar_type(@current_class_idx, ivar_name)
            iv_ctor = ""
            if is_empty_hash_literal(expr_id_iv) == 1 && ivt != "" && ivt != "str_int_hash"
              if ivt == "str_str_hash"
                @needs_str_str_hash = 1
                iv_ctor = "sp_StrStrHash_new()"
              elsif ivt == "int_str_hash"
                @needs_int_str_hash = 1
                iv_ctor = "sp_IntStrHash_new()"
              elsif ivt == "sym_int_hash"
                @needs_sym_int_hash = 1
                iv_ctor = "sp_SymIntHash_new()"
              elsif ivt == "sym_str_hash"
                @needs_sym_str_hash = 1
                iv_ctor = "sp_SymStrHash_new()"
              elsif ivt == "str_poly_hash"
                @needs_str_poly_hash = 1
                iv_ctor = "sp_StrPolyHash_new()"
              elsif ivt == "sym_poly_hash"
                @needs_sym_poly_hash = 1
                iv_ctor = "sp_SymPolyHash_new()"
              end
            end
            if iv_ctor != ""
              @needs_gc = 1
              emit_raw("  " + self_arrow + ivar + " = " + iv_ctor + ";")
            else
              val = compile_expr(expr_id_iv)
              emit_raw("  " + self_arrow + ivar + " = " + val + ";")
            end
          else
            if @nd_type[sid] != "SuperNode"
              # Compile other statements (e.g., method calls like @arr[0] = val)
              compile_stmt(sid)
            end
          end
        }
        pop_scope
        @current_class_idx = -1
      end
    else
      # No own initialize - call parent's if it exists
      if init_ci >= 0
        if init_ci != ci
          parent_name = @cls_names[init_ci]
          # Build param forwarding: forward all constructor params to parent init
          pi_params = @cls_meth_params[init_ci].split("|")
          pi_idx = cls_find_method_direct(init_ci, "initialize")
          pnames = "".split(",")
          if pi_idx >= 0
            if pi_idx < pi_params.length
              pnames = pi_params[pi_idx].split(",")
            end
          end
          fwd = ""
          pk = 0
          while pk < pnames.length
            if pk > 0
              fwd = fwd + ", "
            end
            fwd = fwd + "lv_" + pnames[pk]
            pk = pk + 1
          end
          emit_raw("  sp_" + parent_name + "_initialize((sp_" + parent_name + " *)self" + (fwd != "" ? ", " + fwd : "") + ");")
        end
      end
    end

    if @cls_is_value_type[ci] == 0
      emit_raw("  SP_GC_RESTORE();")
    end
    emit_raw("  return self;")
    emit_raw("}")
    emit_raw("")
    @in_gc_scope = saved_gc_scope

    # Initialize function (for super calls) - always uses *self (pointer)
    if init_idx >= 0
      saved_vt = @cls_is_value_type[ci]
      @cls_is_value_type[ci] = 0
      emit_raw("static inline void sp_" + cname + "_initialize(sp_" + cname + " *self" + init_params_decl(ci) + ") {")
      bodies = @cls_meth_bodies[ci].split(";")
      bid = -1
      if init_idx < bodies.length
        bid = bodies[init_idx].to_i
      end
      if bid >= 0
        @current_class_idx = ci
        all_params = @cls_meth_params[ci].split("|")
        all_ptypes = @cls_meth_ptypes[ci].split("|")
        pnames = "".split(",")
        ptypes = "".split(",")

        if init_idx < all_params.length
          pnames = all_params[init_idx].split(",")
        end
        if init_idx < all_ptypes.length
          ptypes = all_ptypes[init_idx].split(",")
        end
        push_scope
        k = 0
        while k < pnames.length
          pt = "int"
          if k < ptypes.length
            pt = ptypes[k]
          end
          declare_var(pnames[k], pt)
          k = k + 1
        end
        # Declare locals so non-ivar statements (`x = 1`) and
        # expressions that reference them compile.
        declare_method_locals(bid, pnames)
        stmts = get_stmts(bid)
        stmts.each { |sid|
          if @nd_type[sid] != "SuperNode"
            compile_stmt(sid)
          end
        }
        pop_scope
        @current_class_idx = -1
      end
      emit_raw("}")
      emit_raw("")
      @cls_is_value_type[ci] = saved_vt
    end
  end

  def emit_instance_method(ci, mname, pnames, ptypes, rt, bid)
    cname = @cls_names[ci]
    @current_class_idx = ci
    @current_method_name = mname
    @current_method_return = rt
    @current_method_block_param = find_block_param_name(pnames, ptypes)
    @indent = 1
    @in_gc_scope = 0

    midx = cls_find_method_direct(ci, mname)
    if midx >= 0
      if cls_method_has_yield(ci, midx) == 1
        @in_yield_method = 1
      else
        @in_yield_method = 0
      end
    end

    yp = ""
    if @in_yield_method == 1
      yp = yield_params_suffix_cls(ci, midx)
    end
    cm_linkage = method_linkage_named(bid, @in_yield_method, mname)
    if @cls_is_value_type[ci] == 1
      emit_raw(cm_linkage + c_type(rt) + " sp_" + cname + "_" + sanitize_name(mname) + "(sp_" + cname + " self" + build_params_str(pnames, ptypes) + yp + ") {")
    else
      emit_raw(cm_linkage + c_type(rt) + " sp_" + cname + "_" + sanitize_name(mname) + "(sp_" + cname + " *self" + build_params_str(pnames, ptypes) + yp + ") {")
    end

    push_scope
    j = 0
    while j < pnames.length
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      declare_var(pnames[j], pt)
      j = j + 1
    end

    if bid >= 0
      declare_method_locals(bid, pnames)
      if @in_gc_scope == 0
        if @needs_gc == 1
          emit("  SP_GC_SAVE();")
          @in_gc_scope = 1
        end
      end
      if @in_gc_scope == 1
        if @cls_is_value_type[ci] == 0
          emit("  SP_GC_ROOT(self);")
        end
        j = 0
        while j < pnames.length
          if j < ptypes.length
            if type_is_pointer(ptypes[j]) == 1
              emit("  SP_GC_ROOT(lv_" + pnames[j] + ");")
            end
          end
          j = j + 1
        end
      end
      compile_body_return(bid, rt)
    end

    pop_scope
    @current_class_idx = -1
    @current_method_name = ""
    @current_method_block_param = ""
    @in_yield_method = 0
    @indent = 0
    emit_raw("  return " + c_return_default(rt) + ";")
    emit_raw("}")
    emit_raw("")
  end

  def emit_class_level_method(ci, mname, pnames, ptypes, rt, bid)
    cname = @cls_names[ci]
    @current_class_idx = ci
    @current_method_name = mname
    @current_method_return = rt
    @current_method_block_param = find_block_param_name(pnames, ptypes)
    @indent = 1
    @in_gc_scope = 0

    emit_raw("static " + c_type(rt) + " sp_" + cname + "_cls_" + sanitize_name(mname) + "(" + build_params_decl(pnames, ptypes) + ") {")

    push_scope
    j = 0
    while j < pnames.length
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      declare_var(pnames[j], pt)
      j = j + 1
    end

    if bid >= 0
      declare_method_locals(bid, pnames)
      compile_body_return(bid, rt)
    end

    pop_scope
    @current_class_idx = -1
    @current_method_name = ""
    @current_method_block_param = ""
    @indent = 0
    emit_raw("  return " + c_return_default(rt) + ";")
    emit_raw("}")
    emit_raw("")
  end

  def build_params_str(pnames, ptypes)
    result = ""
    j = 0
    while j < pnames.length
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + ", " + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

  def build_params_decl(pnames, ptypes)
    if pnames.length == 0
      return "void"
    end
    result = ""
    j = 0
    while j < pnames.length
      if j > 0
        result = result + ", "
      end
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      result = result + c_type(pt) + " lv_" + pnames[j]
      j = j + 1
    end
    result
  end

  # Builds the trailing portion of a call-args list — each non-empty
  # piece prefixed with ", ", empties skipped. Returns "" when both
  # are empty. Mirrors build_params_str: callers concatenate the
  # result onto a self/recv prefix to form the full arg list.
  def build_call_tail(ca, bp)
    result = ""
    if ca != ""
      result = result + ", " + ca
    end
    if bp != ""
      result = result + ", " + bp
    end
    result
  end

  # ---- Emit top-level methods ----
  def emit_toplevel_methods
    i = 0
    while i < @meth_names.length
      emit_toplevel_method(i)
      i = i + 1
    end
  end

  def emit_toplevel_method(mi)
    mfullname = @meth_names[mi]
    @current_method_name = mfullname
    @current_method_return = @meth_return_types[mi]
    @indent = 1
    @in_main = 0
    @in_gc_scope = 0

    # Check if this is an open class method
    oc_type = ""
    if mfullname.start_with?("__oc_Integer_")
      oc_type = "int"
    end
    if mfullname.start_with?("__oc_String_")
      oc_type = "string"
    end
    if mfullname.start_with?("__oc_Float_")
      oc_type = "float"
    end

    if @meth_has_yield[mi] == 1
      @in_yield_method = 1
    else
      @in_yield_method = 0
    end

    pnames = @meth_param_names[mi].split(",")
    ptypes = @meth_param_types[mi].split(",")
    @current_method_block_param = find_block_param_name(pnames, ptypes)

    yp = ""
    if @meth_has_yield[mi] == 1
      yp = yield_params_suffix(mi)
    end

    if oc_type != ""
      # Open class method: self is primitive type
      rt = @meth_return_types[mi]
      self_ctype = c_type(oc_type)
      pdecl = ""
      if pnames.length > 0
        pdecl = build_params_decl(pnames, ptypes)
        pdecl = self_ctype + " self, " + pdecl
      else
        pdecl = self_ctype + " self"
      end
      emit_raw(method_linkage_named(@meth_body_ids[mi], @meth_has_yield[mi], mfullname) + c_type(rt) + " sp_" + sanitize_name(mfullname) + "(" + pdecl + ") {")
      push_scope
      declare_var("__self_type", oc_type)
    else
      emit_raw(method_linkage_named(@meth_body_ids[mi], @meth_has_yield[mi], mfullname) + c_type(@meth_return_types[mi]) + " sp_" + sanitize_name(mfullname) + "(" + method_params_decl(mi) + yp + ") {")
      push_scope
    end

    j = 0
    while j < pnames.length
      pt = "int"
      if j < ptypes.length
        pt = ptypes[j]
      end
      declare_var(pnames[j], pt)
      j = j + 1
    end

    bid = @meth_body_ids[mi]
    if bid >= 0
      declare_method_locals(bid, pnames)
      if @in_gc_scope == 0
        if @needs_gc == 1
          emit("  SP_GC_SAVE();")
          @in_gc_scope = 1
        end
      end
      if @in_gc_scope == 1
        j = 0
        while j < pnames.length
          if j < ptypes.length
            if type_is_pointer(ptypes[j]) == 1
              emit("  SP_GC_ROOT(lv_" + pnames[j] + ");")
            end
          end
          j = j + 1
        end
      end
      compile_body_return(bid, @meth_return_types[mi])
    end

    rt = @meth_return_types[mi]
    pop_scope
    @current_method_name = ""
    @current_method_block_param = ""
    @in_yield_method = 0
    @indent = 0
    emit_raw("  return " + c_return_default(rt) + ";")
    emit_raw("}")
    emit_raw("")
  end

  def declare_method_locals(bid, params)
    lnames = "".split(",")
    ltypes = "".split(",")

    scan_locals(bid, lnames, ltypes, params)
    # Declare all locals first so block param inference can see them
    j = 0
    while j < lnames.length
      declare_var(lnames[j], ltypes[j])
      j = j + 1
    end
    # Second pass: re-scan with types now in scope for better block param inference
    lnames2 = "".split(",")
    ltypes2 = "".split(",")

    scan_locals(bid, lnames2, ltypes2, params)
    # Update types that may have improved
    j = 0
    while j < lnames2.length
      k = 0
      while k < lnames.length
        if lnames[k] == lnames2[j]
          if ltypes[k] == "int"
            if ltypes2[j] != "int"
              ltypes[k] = ltypes2[j]
              set_var_type(lnames[k], ltypes2[j])
            end
          elsif ltypes[k] == "nil" && is_nullable_pointer_type(ltypes2[j]) == 1
            # `prev = nil` then `prev = obj` — upgrade to obj?
            if is_nullable_type(ltypes2[j]) == 1
              ltypes[k] = ltypes2[j]
            else
              ltypes[k] = ltypes2[j] + "?"
            end
            set_var_type(lnames[k], ltypes[k])
          end
        end
        k = k + 1
      end
      j = j + 1
    end
    # Third pass: upgrade locals passed to lambda-param functions
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
    # Emit declarations and GC rooting for pointer locals
    has_gc_locals = 0
    j = 0
    while j < lnames.length
      if type_is_pointer(ltypes[j]) == 1
        has_gc_locals = 1
      end
      j = j + 1
    end
    if has_gc_locals == 1
      if @needs_gc == 1 && @in_gc_scope == 0
        emit("  SP_GC_SAVE();")
        @in_gc_scope = 1
      end
    end
    j = 0
    while j < lnames.length
      emit("  " + c_type(ltypes[j]) + " lv_" + lnames[j] + " = " + c_default_val(ltypes[j]) + ";")
      j = j + 1
    end
    if has_gc_locals == 1
      if @needs_gc == 1
        emit_gc_roots(lnames, ltypes)
      end
    end
  end

  def emit_gc_roots(lnames, ltypes)
    j = 0
    while j < lnames.length
      if type_is_pointer(ltypes[j]) == 1
        emit("  SP_GC_ROOT(lv_" + lnames[j] + ");")
      end
      j = j + 1
    end
  end

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
    # Parallel to `names`: "1" if this local's current stored type was set
    # by an explicit literal write, "" otherwise. Reset when called with
    # a fresh (empty) names array.
    if names.length == 0
      @scan_literal_flags = "".split(",")
      # Parallel to `names`: "1" if every write to this local so far was
      # an empty `[]` literal — used to defer the array element type
      # until first `push` (issue #58). A subsequent write with a
      # concrete element resets the flag to "".
      @scan_empty_flags = "".split(",")
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
            end
          end
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
          # the local's element type (issue #58).
          if is_empty_array_literal(@nd_expression[nid]) == 1
            @scan_empty_flags.push("1")
          else
            @scan_empty_flags.push("")
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
          ki = 0
          while ki < names.length
            if names[ki] == lname
              if types[ki] != at
                if types[ki] != "poly"
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
              end
            end
          end
        end
      end
    end
    # Issue #58: empty-array param promotion at instance-method call
    # sites — `obj.method(arg)`. Same forward/backward propagation as
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
    # Issue #58: empty-array param promotion at top-level function
    # call sites. Two directions in one place:
    #   (a) Forward: if `arg` is `[]` literal or a local with the
    #       empty flag set, mark @meth_param_empty[mi][k] = "1" so a
    #       later body-promotion pass can refine the param type.
    #   (b) Backward: if @meth_param_types[mi][k] has already been
    #       promoted to a concrete typed-array (str_array, etc.) and
    #       `arg` is a local with the empty flag, upgrade the local's
    #       type to match — this is what propagates the deferred
    #       resolution back to the caller's variable.
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
    # Detect hash value type from h["key"] = val
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "[]="
        recv = @nd_receiver[nid]
        if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode"
          hname = @nd_name[recv]
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length >= 2
              val_type = infer_type(aargs[1])
              if val_type == "string"
                ki = 0
                while ki < names.length
                  if names[ki] == hname
                    if types[ki] == "str_int_hash"
                      types[ki] = "str_str_hash"
                      @needs_str_str_hash = 1
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
                  types.push(ptr_array_elem_type(nrt))
                else
                  types.push("int")
                end
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

  # ---- Main emission ----
  def emit_main
    stmts = get_body_stmts(@root_id)
    emit_raw("typedef struct{const char**data;mrb_int len;}sp_Argv;")
    emit_raw("static sp_Argv sp_argv;")
    emit_raw("")
    emit_raw("int main(int argc,char**argv){")
    emit_raw("  sp_argv.len=argc-1;sp_argv.data=(const char**)malloc(sizeof(const char*)*(argc>1?argc-1:1));{int _i;for(_i=0;_i<sp_argv.len;_i++)sp_argv.data[_i]=sp_str_dup_external(argv[_i+1]);}")
    if @needs_rand == 1
      emit_raw("  srand((unsigned)time(NULL));")
    end
    if @needs_regexp == 1
      emit_raw("  sp_re_init();")
    end

    @in_main = 1
    @indent = 1
    push_scope
    if @needs_gc == 1
      emit("  SP_GC_SAVE();")
    end

    # Pre-declare main locals
    lnames = "".split(",")
    ltypes = "".split(",")

    empty_params = "".split(",")
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            scan_locals(sid, lnames, ltypes, empty_params)
          end
        end
      end
    }

    # Declare vars for second pass to resolve dependent types
    j = 0
    while j < lnames.length
      declare_var(lnames[j], ltypes[j])
      j = j + 1
    end
    # Second pass with vars in scope
    lnames2 = "".split(",")
    ltypes2 = "".split(",")

    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            if @nd_type[sid] != "ModuleNode"
              scan_locals(sid, lnames2, ltypes2, empty_params)
            end
          end
        end
      end
    }
    # Update types that improved in second pass
    j = 0
    while j < lnames2.length
      k = 0
      while k < lnames.length
        if lnames[k] == lnames2[j]
          if ltypes[k] == "int" || ltypes[k] == "nil"
            if ltypes2[j] != "int" && ltypes2[j] != "nil"
              ltypes[k] = ltypes2[j]
              set_var_type(lnames[k], ltypes2[j])
            end
          end
          # Upgrade default array/hash types with more specific ones
          if ltypes[k] == "int_array" && ltypes2[j] != "int_array" && ltypes2[j] != "int"
            ltypes[k] = ltypes2[j]
            set_var_type(lnames[k], ltypes2[j])
          end
          if is_ptr_array_type(ltypes[k]) == 1 && ltypes2[j] != ltypes[k] && is_ptr_array_type(ltypes2[j]) == 1
            ltypes[k] = ltypes2[j]
            set_var_type(lnames[k], ltypes2[j])
          end
          if ltypes[k] == "str_int_hash" && ltypes2[j] == "str_str_hash"
            ltypes[k] = ltypes2[j]
            set_var_type(lnames[k], ltypes2[j])
          end
        end
        k = k + 1
      end
      j = j + 1
    end

    # Update scope with second-pass results before third pass
    j = 0
    while j < lnames.length
      set_var_type(lnames[j], ltypes[j])
      j = j + 1
    end

    # Third pass: re-scan to resolve dependent types (e.g., block params of array-of-array)
    lnames3 = "".split(",")
    ltypes3 = "".split(",")
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            if @nd_type[sid] != "ModuleNode"
              scan_locals(sid, lnames3, ltypes3, empty_params)
            end
          end
        end
      end
    }
    j = 0
    while j < lnames3.length
      k = 0
      while k < lnames.length
        if lnames[k] == lnames3[j]
          if ltypes[k] == "int_array" && ltypes3[j] != "int_array" && ltypes3[j] != "int"
            ltypes[k] = ltypes3[j]
            set_var_type(lnames[k], ltypes3[j])
          end
          if is_tuple_type(ltypes3[j]) == 1 && is_tuple_type(ltypes[k]) == 0
            ltypes[k] = ltypes3[j]
            set_var_type(lnames[k], ltypes3[j])
          end
          if is_tuple_type(ltypes[k]) == 1 && is_tuple_type(ltypes3[j]) == 1 && ltypes[k] != ltypes3[j]
            ltypes[k] = ltypes3[j]
            set_var_type(lnames[k], ltypes3[j])
          end
          if ltypes[k] == "int" && ltypes3[j] != "int" && ltypes3[j] != "nil"
            ltypes[k] = ltypes3[j]
            set_var_type(lnames[k], ltypes3[j])
          end
        end
        k = k + 1
      end
      j = j + 1
    end

    # Fourth pass: upgrade locals passed to lambda-param functions
    j = 0
    while j < lnames.length
      if ltypes[j] == "int"
        # Check all main-level statements for lambda usage
        i2 = 0
        while i2 < stmts.length
          sid2 = stmts[i2]
          if @nd_type[sid2] != "DefNode"
            if @nd_type[sid2] != "ClassNode"
              if @nd_type[sid2] != "ConstantWriteNode"
                if @nd_type[sid2] != "ModuleNode"
                  if param_used_as_lambda(lnames[j], sid2) == 1
                    ltypes[j] = "lambda"
                    set_var_type(lnames[j], "lambda")
                  end
                end
              end
            end
          end
          i2 = i2 + 1
        end
      end
      j = j + 1
    end

    # Bigint promotion: variables with *= in while loops
    detect_bigint_vars(stmts, lnames, ltypes)
    # Update scope with bigint types
    j = 0
    while j < lnames.length
      if ltypes[j] == "bigint"
        set_var_type(lnames[j], "bigint")
      end
      j = j + 1
    end

    vol = ""
    if @needs_setjmp == 1
      vol = "volatile "
    end
    j = 0
    while j < lnames.length
      ctp = c_type(ltypes[j])
      if type_is_pointer(ltypes[j]) == 1
        emit("  " + vol + ctp + "lv_" + lnames[j] + " = " + c_default_val(ltypes[j]) + ";")
        emit("  SP_GC_ROOT(lv_" + lnames[j] + ");")
      else
        emit("  " + vol + ctp + " lv_" + lnames[j] + " = " + c_default_val(ltypes[j]) + ";")
      end
      j = j + 1
    end

    # Constants (initialize global declarations)
    i = 0
    while i < @const_names.length
      old_scope = @current_lexical_scope
      if i < @const_scope_names.length
        @current_lexical_scope = @const_scope_names[i]
      else
        @current_lexical_scope = ""
      end
      val = compile_expr(@const_expr_ids[i])
      @current_lexical_scope = old_scope
      emit("  cst_" + @const_names[i] + " = " + val + ";")
      if type_is_pointer(@const_types[i]) == 1
        emit("  SP_GC_ROOT(cst_" + @const_names[i] + ");")
      end
      i = i + 1
    end

    emit_raw("")

    # Pre-scan: map lambda variable names to their return types
    scan_lambda_ret_types(stmts)

    # Compile main statements
    stmts.each { |sid|
      if @nd_type[sid] != "DefNode"
        if @nd_type[sid] != "ClassNode"
          if @nd_type[sid] != "ConstantWriteNode"
            compile_stmt(sid)
          end
        end
      end
    }

    emit_raw("  return 0;")
    emit_raw("}")

    pop_scope
    @in_main = 0

    # Accumulate lambda and fiber functions into deferred buffer
    if @lambda_funcs != ""
      @deferred_lambda << @lambda_funcs
    end
    if @fiber_funcs != ""
      @deferred_lambda << @fiber_funcs
    end
    0
  end

  # Compile a node for use as a C scalar condition. Value-type objects
  # are passed by value (a struct), and C rejects them as scalars in
  # `if (...)` etc. In Ruby every non-nil/non-false object is truthy,
  # so wrap the expression in a comma operator that evaluates it for
  # side effects then yields 1.
  def compile_cond_expr(nid)
    expr = compile_expr(nid)
    if nid >= 0 && is_value_type_obj(infer_type(nid)) == 1
      return "((" + expr + "), 1)"
    end
    expr
  end

  # ---- Expression compiler ----
  def compile_expr(nid)
    if nid < 0
      return "0"
    end
    t = @nd_type[nid]
    if t == "IntegerNode"
      return @nd_value[nid].to_s
    end
    if t == "FloatNode"
      return @nd_content[nid]
    end
    if t == "StringNode"
      return c_string_literal(@nd_content[nid])
    end
    if t == "SymbolNode"
      return compile_symbol_literal(@nd_content[nid])
    end
    if t == "InterpolatedStringNode"
      return compile_interpolated(nid)
    end
    if t == "NumberedReferenceReadNode"
      num = @nd_value[nid]
      if num >= 1 && num <= 9
        return "(sp_re_captures[" + num.to_s + "] ? sp_re_captures[" + num.to_s + "] : \"\")"
      end
      return "\"\""
    end
    if t == "MatchWriteNode"
      # $1 = ... pattern match
      return compile_expr(@nd_receiver[nid])
    end
    if t == "TrueNode"
      return "TRUE"
    end
    if t == "FalseNode"
      return "FALSE"
    end
    if t == "NilNode"
      return "0"
    end
    if t == "SelfNode"
      return "self"
    end
    if t == "LocalVariableReadNode"
      return fiber_var_ref(@nd_name[nid])
    end
    if t == "InstanceVariableReadNode"
      # Check if we're in a module class method
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          if @current_method_name.start_with?(mmod + "_cls_")
            iname = @nd_name[nid]
            cname3 = mmod + "_" + iname[1, iname.length - 1]
            ci3 = find_const_idx(cname3)
            if ci3 >= 0
              return "cst_" + cname3
            end
          end
        end
        mi3 = mi3 + 1
      end
      return self_arrow + sanitize_ivar(@nd_name[nid])
    end
    if t == "InstanceVariableWriteNode"
      val = compile_expr(@nd_expression[nid])
      # Check if in module method
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          if @current_method_name.start_with?(mmod + "_cls_")
            iname = @nd_name[nid]
            cname3 = mmod + "_" + iname[1, iname.length - 1]
            ci3 = find_const_idx(cname3)
            if ci3 >= 0
              return "(cst_" + cname3 + " = " + val + ")"
            end
          end
        end
        mi3 = mi3 + 1
      end
      return "(" + self_arrow + sanitize_ivar(@nd_name[nid]) + " = " + val + ")"
    end
    if t == "ConstantReadNode"
      if @nd_name[nid] == "ARGV"
        return "sp_argv"
      end
      rname = resolve_const_read_name(@nd_name[nid])
      ci = find_const_idx(rname)
      if ci >= 0
        # Propagate simple literal constants to their use sites.
        lv = const_literal_c_value(ci)
        if lv != ""
          return lv
        end
        return "cst_" + rname
      end
      # Built-in module-like constants (Math, File, ENV, …) and
      # registered classes / modules legitimately reach here as a
      # method-call receiver and don't need their own value at the
      # use site. Any other unresolved constant means the user wrote
      # a name we don't know about — reject it now with a clear
      # NameError-style message instead of emitting a bare C
      # identifier that the C compiler later trips over (issue #75).
      if is_known_constant_name(rname) == 0
        $stderr.puts "Error: uninitialized constant " + rname
        exit(1)
      end
      return rname
    end
    if t == "ConstantPathNode"
      cpname = resolve_const_ref_name(nid)
      if cpname != ""
        ci = find_const_idx(cpname)
        if ci >= 0
          return "cst_" + cpname
        end
      end
      if @nd_receiver[nid] >= 0
        rname = resolve_const_ref_name(@nd_receiver[nid])
        nname = @nd_name[nid]
        # Built-in constants
        if rname == "Float"
          if nname == "INFINITY"
            return "(1.0/0.0)"
          end
          if nname == "NAN"
            return "(0.0/0.0)"
          end
        end
        if rname == "Integer"
          if nname == "MAX"
            return "INT64_MAX"
          end
        end
        if rname == "Math"
          if nname == "PI"
            return "3.14159265358979323846"
          end
          if nname == "E"
            return "2.71828182845904523536"
          end
        end
        if cpname != ""
          return cpname
        end
      end
      return @nd_name[nid]
    end
    if t == "LambdaNode"
      return compile_lambda_expr(nid)
    end
    if t == "CallNode"
      return compile_call_expr(nid)
    end
    if t == "IfNode"
      return compile_if_expr(nid)
    end
    if t == "UnlessNode"
      return compile_unless_expr(nid)
    end
    if t == "AndNode"
      return "(" + compile_expr(@nd_left[nid]) + " && " + compile_expr(@nd_right[nid]) + ")"
    end
    if t == "OrNode"
      return "(" + compile_expr(@nd_left[nid]) + " || " + compile_expr(@nd_right[nid]) + ")"
    end
    if t == "ParenthesesNode"
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          return compile_expr(stmts.last)
        end
      end
      return "0"
    end
    if t == "ArrayNode"
      return compile_array_literal(nid)
    end
    if t == "HashNode"
      return compile_hash_literal(nid)
    end
    if t == "RangeNode"
      return "sp_range_new(" + compile_expr(@nd_left[nid]) + ", " + compile_expr(@nd_right[nid]) + ")"
    end
    if t == "DefinedNode"
      return "\"expression\""
    end
    if t == "RescueModifierNode"
      @needs_setjmp = 1
      tmp = new_temp
      rt = infer_type(@nd_else_clause[nid])
      emit("  " + c_type(rt) + " " + tmp + " = " + c_default_val(rt) + ";")
      emit("  sp_exc_top++;")
      emit("  if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {")
      emit("    " + tmp + " = " + compile_expr(@nd_expression[nid]) + ";")
      emit("    sp_exc_top--;")
      emit("  } else {")
      emit("    sp_exc_top--;")
      emit("    " + tmp + " = " + compile_expr(@nd_else_clause[nid]) + ";")
      emit("  }")
      return tmp
    end
    if t == "XStringNode"
      return "sp_backtick(" + c_string_literal(@nd_content[nid]) + ")"
    end
    if t == "InterpolatedXStringNode"
      interp = compile_interpolated(nid)
      return "sp_backtick(" + interp + ")"
    end
    if t == "GlobalVariableReadNode"
      gname = @nd_name[nid]
      if gname == "$stderr"
        return "0"
      end
      if gname == "$stdout"
        return "0"
      end
      if gname == "$?"
        return "sp_last_status"
      end
      # General global variable
      return sanitize_gvar(gname)
    end
    if t == "SourceLineNode"
      return @nd_value[nid].to_s
    end
    if t == "ArgumentsNode"
      arg_ids = parse_id_list(@nd_args[nid])
      if arg_ids.length > 0
        return compile_expr(arg_ids[0])
      end
      return "0"
    end
    if t == "StatementsNode"
      stmts = parse_id_list(@nd_stmts[nid])
      if stmts.length > 0
        return compile_expr(stmts.last)
      end
      return "0"
    end
    if t == "EmbeddedStatementsNode"
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          return compile_expr(stmts.first)
        end
      end
      return "0"
    end
    if t == "CaseNode"
      # Case as expression: use a temp var and compile each branch as assignment
      rt = infer_type(nid)
      tmp = new_temp
      emit("  " + c_type(rt) + " " + tmp + " = " + c_default_val(rt) + ";")
      pred = @nd_predicate[nid]
      if pred >= 0
        pred_type = infer_type(pred)
        pred_val = compile_expr(pred)
        ptmp = new_temp
        if pred_type == "string"
          emit("  const char *" + ptmp + " = " + pred_val + ";")
        elsif is_obj_type(pred_type) == 1
          # See compile_case_stmt — the temp must be the right pointer
          # type so compile_when_conds can resolve `when ClassName` to
          # a static match. Issue #67.
          bt = base_type(pred_type)
          obj_cname = bt[4, bt.length - 4]
          emit("  sp_" + obj_cname + " *" + ptmp + " = " + pred_val + ";")
        else
          emit("  mrb_int " + ptmp + " = " + pred_val + ";")
        end
        conds = parse_id_list(@nd_conditions[nid])
        k = 0
        while k < conds.length
          wid = conds[k]
          if @nd_type[wid] == "WhenNode"
            kw = "if"
            if k > 0
              kw = "} else if"
            end
            cond_str = compile_when_conds(wid, ptmp, pred_type)
            emit("  " + kw + " (" + cond_str + ") {")
            wbody = @nd_body[wid]
            if wbody >= 0
              ws = get_stmts(wbody)
              if ws.length > 0
                i = 0
                while i < ws.length - 1
                  compile_stmt(ws[i])
                  i = i + 1
                end
                emit("    " + tmp + " = " + compile_expr(ws.last) + ";")
              end
            end
          end
          k = k + 1
        end
      else
        # Bare case (no predicate)
        conds = parse_id_list(@nd_conditions[nid])
        k = 0
        while k < conds.length
          wid = conds[k]
          if @nd_type[wid] == "WhenNode"
            kw = "if"
            if k > 0
              kw = "} else if"
            end
            wconds = parse_id_list(@nd_conditions[wid])
            cexpr = "0"
            if wconds.length > 0
              cexpr = compile_expr(wconds.first)
            end
            emit("  " + kw + " (" + cexpr + ") {")
            wbody = @nd_body[wid]
            if wbody >= 0
              ws = get_stmts(wbody)
              if ws.length > 0
                i = 0
                while i < ws.length - 1
                  compile_stmt(ws[i])
                  i = i + 1
                end
                emit("    " + tmp + " = " + compile_expr(ws.last) + ";")
              end
            end
          end
          k = k + 1
        end
      end
      ec = @nd_else_clause[nid]
      if ec >= 0
        emit("  } else {")
        ebody = @nd_body[ec]
        if ebody >= 0
          es = get_stmts(ebody)
          if es.length > 0
            i = 0
            while i < es.length - 1
              compile_stmt(es[i])
              i = i + 1
            end
            emit("    " + tmp + " = " + compile_expr(es.last) + ";")
          end
        end
      end
      conds2 = parse_id_list(@nd_conditions[nid])
      if conds2.length > 0
        emit("  }")
      end
      return tmp
    end
    "0"
  end

  def c_string_literal(s)
    result = "\""
    i = 0
    while i < s.length
      ch = s[i]
      if ch == bsl
        # Check for Ruby escape sequences
        if i + 1 < s.length
          nch = s[i + 1]
          if nch == "n"
            result = result + bsl_n
            i = i + 2
          else
            if nch == "t"
              result = result + bsl + "t"
              i = i + 2
            else
              if nch == "r"
                result = result + bsl + "r"
                i = i + 2
              else
                if nch == bsl
                  result = result + bsl + bsl
                  i = i + 2
                else
                  if nch == "\""
                    result = result + bsl + "\""
                    i = i + 2
                  else
                    result = result + bsl + bsl
                    i = i + 1
                  end
                end
              end
            end
          end
        else
          result = result + "\\\\"
          i = i + 1
        end
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
        i = i + 1
      end
    end
    # Prepend 0xff marker byte so GC can identify static literals.
    # Return form: (&("\xff" "content")[1]) — same pointer value as the
    # legacy ("\xff" "content" + 1) idiom, but uses array indexing so
    # clang doesn't flag it under -Wstring-plus-int.
    "(&(\"\\xff\" " + result + "\")[1])"
  end

  def compile_interpolated(nid)
    parts = parse_id_list(@nd_parts[nid])
    if parts.length == 0
      return "(&(\"\\xff\")[1])"
    end
    fmt = ""
    arg_exprs = "".split(",")
    parts.each { |pid|
      if @nd_type[pid] == "StringNode"
        fmt = fmt + escape_c_format(@nd_content[pid])
      else
        if @nd_type[pid] == "EmbeddedStatementsNode"
          body = @nd_body[pid]
          if body >= 0
            stmts = get_stmts(body)
            if stmts.length > 0
              inner = stmts.first
              it = infer_type(inner)
              if it == "int"
                fmt = fmt + "%lld"
                arg_exprs.push("(long long)" + compile_expr(inner))
              else
                if it == "float"
                  fmt = fmt + "%g"
                  arg_exprs.push(compile_expr(inner))
                else
                  if it == "string"
                    fmt = fmt + "%s"
                    arg_exprs.push(compile_expr(inner))
                  else
                    if it == "bool"
                      fmt = fmt + "%s"
                      arg_exprs.push("(" + compile_expr(inner) + " ? \"true\" : \"false\")")
                    else
                      if it == "poly"
                        fmt = fmt + "%s"
                        arg_exprs.push("sp_poly_to_s(" + compile_expr(inner) + ")")
                      else
                        fmt = fmt + "%lld"
                        arg_exprs.push("(long long)" + compile_expr(inner))
                      end
                    end
                  end
                end
              end
            end
          end
        end
      end
    }
    result = "sp_sprintf(\"" + fmt + "\""
    j = 0
    while j < arg_exprs.length
      result = result + ", " + arg_exprs[j]
      j = j + 1
    end
    result + ")"
  end

  def escape_c_format(s)
    result = ""
    i = 0
    while i < s.length
      ch = s[i]
      if ch == "%"
        result = result + "%%"
      else
        if ch == bsl
          result = result + bsl + bsl
        else
          if ch == "\""
            result = result + bsl + "\""
          else
            if ch == 10.chr
              result = result + bsl_n
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
    result
  end

  # True if `t` is a GC-allocated pointer that could be swept by
  # sp_gc_collect if held only as a C-stack temp when the collector
  # runs mid-expression.
  def type_needs_transient_root(t)
    if t == ""
      return 0
    end
    if is_nullable_type(t) == 1
      t = base_type(t)
    end
    if t == "string" || t == "mutable_str"
      return 1
    end
    if t == "int_array" || t == "str_array" || t == "float_array" || t == "sym_array"
      return 1
    end
    if t == "str_int_hash" || t == "str_str_hash" || t == "int_str_hash" || t == "sym_int_hash" || t == "sym_str_hash"
      return 1
    end
    if t == "str_poly_hash" || t == "sym_poly_hash"
      return 1
    end
    if is_ptr_array_type(t) == 1
      return 1
    end
    if is_obj_type(t) == 1
      return 1
    end
    if is_tuple_type(t) == 1
      return 1
    end
    if t == "stringio" || t == "fiber" || t == "bigint" || t == "lambda" || t == "poly_array"
      return 1
    end
    0
  end

  # Compile `nid`, and if it's a call expression whose result is a
  # GC-allocated pointer, bind that result to a rooted temp variable
  # so a subsequent mid-expression sp_gc_collect cannot sweep it.
  # For non-call expressions (locals, ivars, literals) rooting is
  # either already in place or unnecessary.
  def compile_expr_gc_rooted(nid)
    val = compile_expr(nid)
    if nid < 0
      return val
    end
    if @nd_type[nid] != "CallNode"
      return val
    end
    t = infer_type(nid)
    if type_needs_transient_root(t) == 0
      return val
    end
    @needs_gc = 1
    tmp = new_temp
    emit("  " + c_type(t) + " " + tmp + " = " + val + ";")
    emit("  SP_GC_ROOT(" + tmp + ");")
    tmp
  end

  def compile_arg0(nid)
    args_id = @nd_arguments[nid]
    if args_id >= 0
      arg_ids = get_args(args_id)
      if arg_ids.length > 0
        return compile_expr(arg_ids[0])
      end
    end
    "0"
  end

  # Like compile_arg0, but converts symbol-typed arg to const char *
  # (sp_sym_to_s wrap). Use for callsites that need a string key.
  def compile_str_arg0(nid)
    args_id = @nd_arguments[nid]
    if args_id >= 0
      arg_ids = get_args(args_id)
      if arg_ids.length > 0
        return compile_expr_as_string(arg_ids[0])
      end
    end
    "0"
  end


  # --- Fiber capture helpers ---

  def fiber_capture_index(name)
    i = 0
    while i < @fiber_captures.length
      if @fiber_captures[i] == name
        return i
      end
      i = i + 1
    end
    -1
  end

  def proc_capture_index(name)
    i = 0
    while i < @proc_captures.length
      if @proc_captures[i] == name
        return i
      end
      i = i + 1
    end
    -1
  end

  def heap_promoted_cell(name)
    i = 0
    while i < @heap_promoted_names.length
      if @heap_promoted_names[i] == name
        return @heap_promoted_cells[i]
      end
      i = i + 1
    end
    ""
  end

  def fiber_var_ref(name)
    if @in_fiber_body == 1
      if fiber_capture_index(name) >= 0
        return "(*_cap->" + name + ")"
      end
    end
    if @in_proc_body == 1
      if proc_capture_index(name) >= 0
        return "(*_cap->" + name + ")"
      end
      # Inside a proc body, captures only come via _cap->. Heap-promoted
      # cells from outer functions are not visible here.
      return "lv_" + name
    end
    cell = heap_promoted_cell(name)
    if cell != "" && find_var_type(name) != ""
      return "(*" + cell + ")"
    end
    "lv_" + name
  end


  def scan_fiber_free_vars(nid, params, locals, free_vars, free_var_types)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "LocalVariableReadNode" || t == "LocalVariableTargetNode"
      vn = @nd_name[nid]
      if not_in(vn, params) == 1 && not_in(vn, locals) == 1 && not_in(vn, free_vars) == 1
        vt = find_var_type(vn)
        if vt != ""
          free_vars.push(vn)
          free_var_types.push(vt)
        end
      end
      return
    end
    if t == "LocalVariableWriteNode"
      vn = @nd_name[nid]
      if not_in(vn, params) == 1 && not_in(vn, locals) == 1 && not_in(vn, free_vars) == 1
        vt = find_var_type(vn)
        if vt != ""
          free_vars.push(vn)
          free_var_types.push(vt)
        end
      end
      # Also scan the expression
      scan_fiber_free_vars(@nd_expression[nid], params, locals, free_vars, free_var_types)
      return
    end
    if t == "LocalVariableOperatorWriteNode"
      vn = @nd_name[nid]
      if not_in(vn, params) == 1 && not_in(vn, locals) == 1 && not_in(vn, free_vars) == 1
        vt = find_var_type(vn)
        if vt != ""
          free_vars.push(vn)
          free_var_types.push(vt)
        end
      end
      scan_fiber_free_vars(@nd_expression[nid], params, locals, free_vars, free_var_types)
      return
    end
    # Stop at nested lambda/fiber boundaries (they handle their own captures)
    if t == "LambdaNode"
      return
    end
    if t == "CallNode"
      # Stop at nested Fiber.new block bodies
      mn = @nd_name[nid]
      if mn == "new"
        rv = @nd_receiver[nid]
        if rv >= 0 && constructor_class_name(rv) == "Fiber"
          return
        end
      end
    end
    # Recurse into children (follow scan_locals_children pattern)
    if @nd_body[nid] >= 0
      scan_fiber_free_vars(@nd_body[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_receiver[nid] >= 0
      scan_fiber_free_vars(@nd_receiver[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_expression[nid] >= 0
      scan_fiber_free_vars(@nd_expression[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_predicate[nid] >= 0
      scan_fiber_free_vars(@nd_predicate[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_subsequent[nid] >= 0
      scan_fiber_free_vars(@nd_subsequent[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_else_clause[nid] >= 0
      scan_fiber_free_vars(@nd_else_clause[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_arguments[nid] >= 0
      scan_fiber_free_vars(@nd_arguments[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_rescue_clause[nid] >= 0
      scan_fiber_free_vars(@nd_rescue_clause[nid], params, locals, free_vars, free_var_types)
    end
    if @nd_ensure_clause[nid] >= 0
      scan_fiber_free_vars(@nd_ensure_clause[nid], params, locals, free_vars, free_var_types)
    end
    # Stmts list
    stmts_list = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts_list.length
      scan_fiber_free_vars(stmts_list[k], params, locals, free_vars, free_var_types)
      k = k + 1
    end
    # Args list
    args_list = parse_id_list(@nd_args[nid])
    k = 0
    while k < args_list.length
      scan_fiber_free_vars(args_list[k], params, locals, free_vars, free_var_types)
      k = k + 1
    end
    # Conditions list
    conds_list = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds_list.length
      scan_fiber_free_vars(conds_list[k], params, locals, free_vars, free_var_types)
      k = k + 1
    end
    # Elements list
    elems_list = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems_list.length
      scan_fiber_free_vars(elems_list[k], params, locals, free_vars, free_var_types)
      k = k + 1
    end
    # Block body (for non-Fiber.new blocks)
    blk = @nd_block[nid]
    if blk >= 0
      if @nd_body[blk] >= 0
        scan_fiber_free_vars(@nd_body[blk], params, locals, free_vars, free_var_types)
      end
    end
  end

  def compile_fiber_new(nid)
    @needs_fiber = 1
    blk = @nd_block[nid]
    if blk < 0
      return "NULL"
    end
    # Get block parameter name
    bp = ""
    bparams = @nd_parameters[blk]
    if bparams >= 0
      # BlockParametersNode → inner ParametersNode
      inner = @nd_parameters[bparams]
      if inner >= 0
        reqs = parse_id_list(@nd_requireds[inner])
        if reqs.length > 0
          bp = @nd_name[reqs[0]]
        end
      end
      if bp == ""
        # Try direct requireds (in case it's ParametersNode directly)
        reqs = parse_id_list(@nd_requireds[bparams])
        if reqs.length > 0
          bp = @nd_name[reqs[0]]
        end
      end
    end

    body = @nd_body[blk]
    # Scan all variables referenced in the body
    all_names = "".split(",")
    all_types = "".split(",")
    all_plist = "".split(",")
    if bp != ""
      all_plist.push(bp)
    end
    if body >= 0
      scan_locals(body, all_names, all_types, all_plist)
    end

    # Split into captures (exist in outer scope) vs true locals
    free_vars = "".split(",")
    free_var_types = "".split(",")
    local_names = "".split(",")
    local_types = "".split(",")
    k = 0
    while k < all_names.length
      outer_type = find_var_type(all_names[k])
      if outer_type != ""
        # Variable exists in outer scope → capture
        free_vars.push(all_names[k])
        free_var_types.push(outer_type)
      else
        # True local variable
        local_names.push(all_names[k])
        local_types.push(all_types[k])
      end
      k = k + 1
    end

    # Also scan for read-only captures (variables read but not written in body)
    if body >= 0
      scan_fiber_free_vars(body, all_plist, all_names, free_vars, free_var_types)
    end

    fid = @fiber_counter
    @fiber_counter = @fiber_counter + 1
    fname = "_fiber_body_" + fid.to_s
    cap_name = "_fiber_cap_" + fid.to_s

    # Build capture struct typedef if needed
    cap_typedef = ""
    if free_vars.length > 0
      cap_typedef = "typedef struct {"
      k = 0
      while k < free_vars.length
        cap_typedef = cap_typedef + " " + c_type(free_var_types[k]) + " *" + free_vars[k] + ";"
        k = k + 1
      end
      cap_typedef = cap_typedef + " } " + cap_name + ";" + 10.chr
    end

    # Compile fiber body function
    fbody = "static void " + fname + "(sp_Fiber *_fb) {" + 10.chr
    if free_vars.length > 0
      fbody = fbody + "  " + cap_name + " *_cap = (" + cap_name + "*)_fb->user_data;" + 10.chr
    end
    if bp != ""
      fbody = fbody + "  sp_RbVal lv_" + bp + " = _fb->resumed_value;" + 10.chr
    end

    # Save/restore compiler state
    saved_out = @out_lines
    saved_indent = @indent
    saved_in_fiber_body = @in_fiber_body
    saved_fiber_captures = @fiber_captures
    saved_fiber_capture_types = @fiber_capture_types
    saved_hp_names_len = @heap_promoted_names.length
    saved_hp_cells_len = @heap_promoted_cells.length
    @out_lines = "".split(",")
    @indent = 1
    @in_fiber_body = 1
    @fiber_captures = free_vars
    @fiber_capture_types = free_var_types

    push_scope
    if bp != ""
      declare_var(bp, "poly")
    end
    if body >= 0
      # Declare only true locals (not captured vars)
      lk = 0
      while lk < local_names.length
        declare_var(local_names[lk], local_types[lk])
        emit("  " + c_type(local_types[lk]) + " lv_" + local_names[lk] + " = " + c_default_val(local_types[lk]) + ";")
        lk = lk + 1
      end

      stmts = get_stmts(body)
      if stmts.length > 0
        i = 0
        while i < stmts.length - 1
          compile_stmt(stmts[i])
          i = i + 1
        end
        last = stmts.last
        # Compile last as statement for side effects, then capture value
        last_type = infer_type(last)
        if last_type == "void"
          # Side-effect-only stmt (puts, print, etc.) — compile_expr would drop the call
          compile_stmt(last)
          emit("  _fb->yielded_value = sp_box_nil();")
        else
          if @nd_type[last] == "LocalVariableWriteNode" || @nd_type[last] == "LocalVariableOperatorWriteNode"
            compile_stmt(last)
          end
          last_val = compile_expr(last)
          emit("  _fb->yielded_value = " + box_val_to_poly(last_val, last_type) + ";")
        end
      end
    end
    pop_scope

    fbody = fbody + @out_lines.join(10.chr) + 10.chr
    fbody = fbody + "}" + 10.chr
    @fiber_funcs << cap_typedef
    @fiber_funcs << fbody

    @out_lines = saved_out
    @indent = saved_indent
    @in_fiber_body = saved_in_fiber_body
    @fiber_captures = saved_fiber_captures
    @fiber_capture_types = saved_fiber_capture_types
    # Restore heap promoted lists to saved length
    while @heap_promoted_names.length > saved_hp_names_len
      @heap_promoted_names.pop
    end
    while @heap_promoted_cells.length > saved_hp_cells_len
      @heap_promoted_cells.pop
    end

    # If no captures, return simple expression
    if free_vars.length == 0
      return "sp_Fiber_new(" + fname + ")"
    end

    # Heap-promote captured variables (allocate cells if not already promoted)
    k = 0
    while k < free_vars.length
      vn = free_vars[k]
      already_promoted = 0
      # Check outer scope heap promotions
      ci = 0
      while ci < @heap_promoted_names.length
        if @heap_promoted_names[ci] == vn
          already_promoted = 1
        end
        ci = ci + 1
      end
      # Check if variable is already a heap pointer from enclosing fiber capture
      if already_promoted == 0 && @in_fiber_body == 1 && fiber_capture_index(vn) >= 0
        # Reuse the enclosing fiber's capture pointer (_cap->vn is already a heap cell)
        cell = "_cap->" + vn
        @heap_promoted_names.push(vn)
        @heap_promoted_cells.push(cell)
        already_promoted = 1
      end
      if already_promoted == 0
        cell = "_hcell_" + vn + "_" + fid.to_s
        ct = c_type(free_var_types[k])
        emit("  " + ct + " *" + cell + " = (" + ct + "*)sp_gc_alloc(sizeof(" + ct + "), NULL, NULL);")
        emit("  *" + cell + " = " + fiber_var_ref(vn) + ";")
        @heap_promoted_names.push(vn)
        @heap_promoted_cells.push(cell)
      end
      k = k + 1
    end

    # Heap-allocate capture struct
    tmp_fb = "_tmpfb_" + fid.to_s
    cap_ptr = "_cap_ptr_" + fid.to_s
    emit("  " + cap_name + " *" + cap_ptr + " = (" + cap_name + "*)sp_gc_alloc(sizeof(" + cap_name + "), NULL, NULL);")
    k = 0
    while k < free_vars.length
      vn = free_vars[k]
      # Find the cell for this variable
      ci = 0
      cell = ""
      while ci < @heap_promoted_names.length
        if @heap_promoted_names[ci] == vn
          cell = @heap_promoted_cells[ci]
        end
        ci = ci + 1
      end
      emit("  " + cap_ptr + "->" + vn + " = " + cell + ";")
      k = k + 1
    end
    emit("  sp_Fiber *" + tmp_fb + " = sp_Fiber_new(" + fname + ");")
    emit("  " + tmp_fb + "->user_data = " + cap_ptr + ";")
    tmp_fb
  end

  def compile_call_expr(nid)
    mname = @nd_name[nid]
    recv = @nd_receiver[nid]

    # Fiber.new { block }
    if mname == "new" && recv >= 0
      if constructor_class_name(recv) == "Fiber"
        return compile_fiber_new(nid)
      end
    end
    # fiber.resume(val)
    if mname == "resume" && recv >= 0
      rt2 = base_type(infer_type(recv))
      if rt2 == "fiber"
        rc = compile_expr_gc_rooted(recv)
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length > 0
            return "sp_Fiber_resume((sp_Fiber *)(" + rc + "), " + box_expr_to_poly(arg_ids[0]) + ")"
          end
        end
        return "sp_Fiber_resume((sp_Fiber *)(" + rc + "), sp_box_nil())"
      end
    end
    # Fiber.yield(val)
    if mname == "yield" && recv >= 0
      if constructor_class_name(recv) == "Fiber"
        @needs_fiber = 1
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length > 0
            return "sp_Fiber_yield(" + box_expr_to_poly(arg_ids[0]) + ")"
          end
        end
        return "sp_Fiber_yield(sp_box_nil())"
      end
    end
    # fiber.alive?
    if mname == "alive?" && recv >= 0
      rt2 = base_type(infer_type(recv))
      if rt2 == "fiber"
        rc = compile_expr_gc_rooted(recv)
        return "sp_Fiber_alive((sp_Fiber *)(" + rc + "))"
      end
    end
    # fiber.transfer(val)
    if mname == "transfer" && recv >= 0
      rt2 = base_type(infer_type(recv))
      if rt2 == "fiber"
        rc = compile_expr_gc_rooted(recv)
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length > 0
            return "sp_Fiber_transfer((sp_Fiber *)(" + rc + "), " + box_expr_to_poly(arg_ids[0]) + ")"
          end
        end
        return "sp_Fiber_transfer((sp_Fiber *)(" + rc + "), sp_box_nil())"
      end
    end
    # Fiber.current
    if mname == "current" && recv >= 0
      if constructor_class_name(recv) == "Fiber"
        @needs_fiber = 1
        return "sp_fiber_current"
      end
    end

    # regex.match? / regex.match / regex =~ str  — receiver is the regex
    # (typically a constant referring to a /…/ literal). Dispatched here
    # rather than compile_string_method_expr, which wants a string
    # receiver.
    if recv >= 0 && (mname == "match?" || mname == "=~" || mname == "match")
      ridx = find_regexp_index(recv)
      if ridx >= 0
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length > 0
            sc = compile_expr(arg_ids[0])
            if mname == "match?"
              return "sp_re_match_p(sp_re_pat_" + ridx.to_s + ", " + sc + ")"
            end
            return "sp_re_match(sp_re_pat_" + ridx.to_s + ", " + sc + ")"
          end
        end
      end
    end

    # No receiver
    if recv < 0
      return compile_no_recv_call_expr(nid, mname)
    end

    # Lambda calls
    rt = infer_type(recv)
    if rt == "lambda"
      r = compile_lambda_call_expr(nid, mname, recv)
      if r != ""
        return r
      end
    end

    # Operator on object with custom method
    if is_operator_name(mname) == 1
      r = compile_obj_operator_expr(nid, mname, recv)
      if r != ""
        return r
      end
    end

    # .call on method reference or proc
    if mname == "call"
      r = compile_dot_call_expr(nid, recv)
      if r != ""
        return r
      end
    end

    # Operators
    r = compile_operator_expr(nid, mname, recv)
    if r != ""
      return r
    end

    # .new
    if mname == "new"
      r = compile_constructor_expr(nid, recv)
      if r != ""
        return r
      end
    end

    recv_type = infer_type(recv)
    # Nullable receiver: dispatch identically to the base type. The
    # null check is the caller's responsibility, matching Ruby's
    # NoMethodError semantics on a nil receiver. Without this, a
    # local typed `string?` (e.g. an opt param `f = nil` later passed
    # a string) misses every per-type dispatcher and falls through
    # to the "0" fallback. Issue #60.
    if is_nullable_type(recv_type) == 1
      recv_type = base_type(recv_type)
    end
    rc = compile_expr_gc_rooted(recv)
    # Root receiver if it may be collected during argument evaluation
    if expr_may_gc(recv) == 1 && type_is_pointer(recv_type) == 1
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        has_gc_arg = 0
        ak = 0
        while ak < aargs.length
          if expr_may_gc(aargs[ak]) == 1
            has_gc_arg = 1
          end
          ak = ak + 1
        end
        if has_gc_arg == 1
          tmp = new_temp
          emit("  " + c_type(recv_type) + " " + tmp + " = " + rc + ";")
          emit("  SP_GC_ROOT(" + tmp + ");")
          rc = tmp
        end
      end
    end

    # StringIO methods
    if recv_type == "stringio"
      r = compile_stringio_method_expr(nid, mname, rc)
      if r != ""
        return r
      end
    end

    # Static intern optimization: "literal".to_sym / .intern where the
    # string content is already in @sym_names becomes a compile-time
    # constant (SPS_<name> or ((sp_sym)<idx>)), avoiding the runtime
    # sp_sym_intern strcmp loop and dynamic pool allocation.
    if recv_type == "string" && (mname == "to_sym" || mname == "intern")
      if recv >= 0 && @nd_type[recv] == "StringNode"
        sname = @nd_content[recv]
        if sym_name_index(sname) >= 0
          return compile_symbol_literal(sname)
        end
      end
    end

    # String methods
    if recv_type == "string"
      r = compile_string_method_expr(nid, mname, rc)
      if r != ""
        return r
      end
    end

    # Symbol methods
    if recv_type == "symbol"
      r = compile_symbol_method_expr(nid, mname, rc)
      if r != ""
        return r
      end
    end

    # Mutable string methods: delegate to string methods via ->data
    if recv_type == "mutable_str"
      if mname == "length" || mname == "size"
        return "sp_String_length(" + rc + ")"
      end
      if mname == "dup"
        return "sp_String_dup(" + rc + ")"
      end
      if mname == "to_s"
        return rc + "->data"
      end
      # For all other string methods, convert via ->data
      r = compile_string_method_expr(nid, mname, rc + "->data")
      if r != ""
        return r
      end
    end

    # Range methods
    if recv_type == "range"
      r = compile_range_method_expr(nid, mname, rc)
      if r != ""
        return r
      end
    end

    # Integer methods
    if recv_type == "int"
      r = compile_int_method_expr(nid, mname, rc)
      if r != ""
        return r
      end
    end

    # Float methods
    if recv_type == "float"
      r = compile_float_method_expr(nid, mname, rc)
      if r != ""
        return r
      end
    end

    # Bigint methods
    if recv_type == "bigint"
      if mname == "to_s"
        return "sp_bigint_to_s(" + rc + ")"
      end
      if mname == "to_i"
        return "sp_bigint_to_int(" + rc + ")"
      end
    end

    # Bool methods
    if recv_type == "bool"
      if mname == "to_s"
        return "(" + rc + " ? \"true\" : \"false\")"
      end
      if mname == "inspect"
        return "(" + rc + " ? \"true\" : \"false\")"
      end
    end

    # nil methods (receiver inferred as "nil" — only .inspect and .to_s
    # need an expression-level answer; other nil methods like .nil? are
    # already handled earlier.)
    if recv_type == "nil"
      if mname == "inspect"
        return "\"nil\""
      end
      if mname == "to_s"
        return "\"\""
      end
    end

    # Tuple methods
    if is_tuple_type(recv_type) == 1
      if mname == "[]"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            if @nd_type[aargs[0]] == "IntegerNode"
              idx = @nd_value[aargs[0]]
              return rc + "->_" + idx.to_s
            end
            idx_expr = compile_expr(aargs[0])
            return rc + "->_" + idx_expr
          end
        end
      end
      if mname == "first"
        return rc + "->_0"
      end
      if mname == "last"
        arity = tuple_arity(recv_type)
        return rc + "->_" + (arity - 1).to_s
      end
      if mname == "length" || mname == "size"
        return tuple_arity(recv_type).to_s
      end
    end

    # Array methods
    r = compile_array_method_expr(nid, mname, rc, recv_type)
    if r != ""
      return r
    end

    # Hash methods
    r = compile_hash_method_expr(nid, mname, rc, recv_type)
    if r != ""
      return r
    end

    # map/select/reject/reduce as expression
    r = compile_enumerable_expr(nid, mname)
    if r != ""
      return r
    end

    # Constant receiver (ARGV, Math, File, Time, ENV, Dir, Module, Class)
    r = compile_constant_recv_expr(nid, mname, recv, rc)
    if r != ""
      return r
    end

    # to_a on range
    if mname == "to_a"
      r = compile_to_a_range_expr(nid, recv)
      if r != ""
        return r
      end
    end

    # Open class method dispatch on built-in types
    r = compile_open_class_dispatch_expr(nid, mname, rc, recv_type)
    if r != ""
      return r
    end

    # Poly method calls
    if recv_type == "poly"
      return compile_poly_method_call(nid, rc, mname)
    end

    # is_a? / respond_to? / nil? / frozen? / positive? / negative?
    r = compile_introspection_expr(nid, mname, rc, recv_type)
    if r != ""
      return r
    end

    # Object method calls
    r = compile_object_method_expr(nid, mname, rc, recv_type)
    if r != ""
      return r
    end

    # Fallback: int-to-class dispatch
    r = compile_int_class_fallback_expr(nid, mname, rc, recv_type)
    if r != ""
      return r
    end

    "0"
  end

  def compile_no_recv_call_expr(nid, mname)
    # catch as expression
    if mname == "catch"
      if @nd_block[nid] >= 0
        return compile_catch_expr(nid)
      end
    end
    if mname == "block_given?"
      # &block parameter form takes priority: when the enclosing method
      # declares `def m(&block)`, the block is bound to `lv_block`, not
      # the implicit yield slot — so check the explicit param first.
      # `body_has_yield` flags any method containing block_given? as a
      # yield-method, which would otherwise route through the `_block`
      # slot and miss the actually-bound `&block` param.
      if @current_method_block_param != ""
        return "(lv_" + @current_method_block_param + " != NULL)"
      end
      if @in_yield_method == 1
        return "(_block != NULL)"
      end
      return "0"
    end
    if mname == "system"
      @needs_system = 1
      return "({ fflush(stdout); sp_last_status = system(" + compile_arg0(nid) + "); sp_last_status == 0; })"
    end
    if mname == "__method__"
      return "\"" + @current_method_name + "\""
    end
    if mname == "Integer"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        if arg_ids.length > 0
          a0 = arg_ids[0]
          # Handle OrNode: Integer(ARGV[0] || default)
          if @nd_type[a0] == "OrNode"
            lt = infer_type(@nd_left[a0])
            rt = infer_type(@nd_right[a0])
            if lt == "string" or lt == "argv"
              # Bind the left-hand expression to a temp so it's evaluated
              # only once, and so GCC's nonnull analysis can see that the
              # strtoll call sits in the truthy branch of the same test.
              lc = compile_expr(@nd_left[a0])
              rc2 = compile_expr(@nd_right[a0])
              tmp = new_temp
              if rt == "int"
                return "({ const char *" + tmp + " = " + lc + "; " + tmp + " ? (mrb_int)strtoll(" + tmp + ", NULL, 10) : " + rc2 + "; })"
              else
                return "({ const char *" + tmp + " = " + lc + "; " + tmp + " ? (mrb_int)strtoll(" + tmp + ", NULL, 10) : (mrb_int)strtoll(" + rc2 + ", NULL, 10); })"
              end
            end
          end
          at = infer_type(a0)
          if at == "string"
            return "(mrb_int)strtoll(" + compile_expr(a0) + ", NULL, 10)"
          end
          if at == "argv"
            return "(mrb_int)strtoll(" + compile_expr(a0) + ", NULL, 10)"
          end
        end
      end
      return "(mrb_int)(" + compile_arg0(nid) + ")"
    end
    if mname == "Float"
      return "(mrb_float)(" + compile_arg0(nid) + ")"
    end
    if mname == "proc"
      if @nd_block[nid] >= 0
        return compile_proc_literal(nid)
      end
    end
    if mname == "method"
      # method(:name) - record the method reference
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        if arg_ids.length >= 1
          mref = @nd_content[arg_ids[0]]
          if mref == ""
            mref = @nd_name[arg_ids[0]]
          end
          # Return a placeholder - the actual dispatch happens in .call
          # We store this in the parent LocalVariableWriteNode handler
          @pending_method_ref = mref
          return "0 /* method:" + mref + " */"
        end
      end
      return "0"
    end
    if mname == "p"
      # p(val) -> puts(val.inspect). For most types the output matches
      # puts, but symbols need ":name" and strings need quoting.
      compile_p(nid)
      return "0"
    end
    if mname == "srand"
      @needs_rand = 1
      emit("  srand((unsigned)" + compile_arg0(nid) + ");")
      return "0"
    end
    if mname == "sleep"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        emit("  sleep((unsigned)" + compile_arg0(nid) + ");")
      end
      return "0"
    end
    if mname == "gets" || mname == "readline"
      return "sp_gets()"
    end
    if mname == "readlines"
      @needs_str_array = 1
      @needs_gc = 1
      return "sp_readlines()"
    end
    if mname == "rand"
      @needs_rand = 1
      args_id = @nd_arguments[nid]
      if args_id >= 0
        return "((mrb_int)(rand() % (int)" + compile_arg0(nid) + "))"
      end
      return "((mrb_int)rand())"
    end
    if mname == "raise"
      @needs_setjmp = 1
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        if arg_ids.length >= 1
          emit("  sp_raise(" + compile_expr(arg_ids[0]) + ");")
        end
      else
        emit("  sp_raise(\"RuntimeError\");")
      end
      return "0"
    end
    if mname == "format"
      return compile_sprintf_call(nid)
    end
    if mname == "sprintf"
      return compile_sprintf_call(nid)
    end
    if mname == "putc"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        if arg_ids.length > 0
          at = infer_type(arg_ids[0])
          if at == "int"
            return "(putchar((char)" + compile_expr(arg_ids[0]) + "), 0)"
          else
            return "(putchar(" + compile_expr(arg_ids[0]) + "[0]), 0)"
          end
        end
      end
      return "0"
    end
    mi = find_method_idx(mname)
    if mi >= 0
      yargs = ""
      if @meth_has_yield[mi] == 1
        yargs = ", NULL, NULL"
      end
      # Check if function has a &block param and caller provides a block.
      # Ruby syntax requires `&block` to be the trailing param — a proc-typed
      # slot in any other position is a positional proc argument, not a block
      # param. Mirrors cls_method_has_block_param.
      ptypes = @meth_param_types[mi].split(",")
      has_block_param = (ptypes.length > 0 && ptypes.last == "proc") ? 1 : 0
      if has_block_param == 1
        # Forward the call site's literal block or `&proc_var` into the
        # callee's &block slot. Use compile_call_args_with_defaults so
        # optional positional params get their defaults filled in, and
        # pass omit_trailing=1 so the &block slot isn't default-padded
        # with "0" — we append the actual proc explicitly below.
        block_proc = block_forward_expr(nid)
        if block_proc != ""
          ca = compile_call_args_with_defaults(nid, mi, 1)
          if ca == ""
            return "sp_" + sanitize_name(mname) + "(" + block_proc + ")"
          end
          return "sp_" + sanitize_name(mname) + "(" + ca + ", " + block_proc + ")"
        end
      end
      return "sp_" + sanitize_name(mname) + "(" + compile_call_args_with_defaults(nid, mi) + yargs + ")"
    end
    # Check if we're inside an open class method: implicit self.method
    st = find_var_type("__self_type")
    if st != ""
      # Redirect as self.mname - string methods
      if st == "string"
        if mname == "upcase"
          return "sp_str_upcase(self)"
        end
        if mname == "downcase"
          return "sp_str_downcase(self)"
        end
        if mname == "length"
          return "sp_str_length(self)"
        end
        if mname == "strip"
          return "sp_str_strip(self)"
        end
        if mname == "chomp"
          return "sp_str_chomp(self)"
        end
        if mname == "to_i"
          return "((mrb_int)atoll(self))"
        end
        if mname == "split"
          @needs_str_array = 1
          return "sp_str_split(self, " + compile_arg0(nid) + ")"
        end
        if mname == "include?"
          return "sp_str_include(self, " + compile_arg0(nid) + ")"
        end
        if mname == "gsub"
          args_id = @nd_arguments[nid]
          arg1 = "\"\""
          if args_id >= 0
            a = get_args(args_id)
            if a.length >= 2
              arg1 = compile_expr(a[1])
            end
          end
          return "sp_str_gsub(self, " + compile_arg0(nid) + ", " + arg1 + ")"
        end
      end
      # int methods
      if st == "int"
        if mname == "to_s"
          return "sp_int_to_s(self)"
        end
        if mname == "to_f"
          return "(mrb_float)(self)"
        end
        if mname == "abs"
          return "((self) < 0 ? -(self) : (self))"
        end
      end
      # float methods
      if st == "float"
        if mname == "to_i"
          return "(mrb_int)(self)"
        end
        if mname == "to_s"
          return "sp_float_to_s(self)"
        end
      end
    end
    if @current_class_idx >= 0
      cidx = cls_find_method(@current_class_idx, mname)
      if cidx >= 0
        owner = find_method_owner(@current_class_idx, mname)
        # Look up the method's owning class so we can fill in defaults
        # from @cls_meth_defaults (issue #49) and check for a &block slot.
        owner_ci = find_class_idx(owner)
        owner_midx = -1
        if owner_ci >= 0
          owner_midx = cls_find_method_direct(owner_ci, mname)
        end
        # Omit the trailing &block slot from default-padding when the
        # callee declares one — we'll fill it explicitly from the
        # call site's literal block below.
        has_proc = cls_method_has_block_param(owner_ci, owner_midx)
        ca = ""
        if owner_midx >= 0
          ca = compile_typed_call_args(nid, owner_ci, owner_midx, has_proc)
        else
          ca = compile_call_args(nid)
        end
        bp = ""
        if has_proc == 1
          bp = block_forward_expr(nid)
          if bp == ""
            # The callee declares &block but the call site provides
            # none — fill the slot with NULL so the C call has the
            # right arity.
            bp = "0"
          end
        end
        return "sp_" + owner + "_" + sanitize_name(mname) + "(self" + build_call_tail(ca, bp) + ")"
      end
      # Check attr_readers (bare method call like `x` meaning self.x)
      readers = @cls_attr_readers[@current_class_idx].split(";")
      rk = 0
      while rk < readers.length
        if readers[rk] == mname
          return self_arrow + sanitize_ivar(mname)
        end
        rk = rk + 1
      end
    end
    return "0"
    "0"
  end

  def compile_lambda_call_expr(nid, mname, recv)
    rc = compile_expr_gc_rooted(recv)
    # Determine return type unboxing
    ret_type = ""
    if @nd_type[recv] == "LocalVariableReadNode"
      ret_type = lambda_var_ret_type(@nd_name[recv])
    end
    if mname == "[]" || mname == "call"
      call_expr = ""
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length > 0
          ac = wrap_as_sp_val(aargs.first)
          call_expr = "sp_lam_call(" + rc + ", " + ac + ")"
        end
      end
      if call_expr == ""
        call_expr = "sp_lam_call(" + rc + ", &sp_lam_nil_val)"
      end
      return lam_unbox(call_expr, ret_type)
    end
    ""
  end

  def compile_obj_operator_expr(nid, mname, recv)
    lt = infer_type(recv)
    if is_obj_type(lt) == 1
      cname = lt[4, lt.length - 4]
      ci = find_class_idx(cname)
      if ci >= 0
        owner = find_method_owner(ci, mname)
        if owner != ""
          ca = compile_call_args(nid)
          rc = compile_expr_gc_rooted(recv)
          if owner == cname
            if ca != ""
              return "sp_" + owner + "_" + sanitize_name(mname) + "(" + rc + ", " + ca + ")"
            else
              return "sp_" + owner + "_" + sanitize_name(mname) + "(" + rc + ")"
            end
          else
            if ca != ""
              return "sp_" + owner + "_" + sanitize_name(mname) + "((sp_" + owner + " *)" + rc + ", " + ca + ")"
            else
              return "sp_" + owner + "_" + sanitize_name(mname) + "((sp_" + owner + " *)" + rc + ")"
            end
          end
        else
          # Check if class has <=> (Comparable) for comparison operators
          cmp_owner = find_method_owner(ci, "<=>")
          if cmp_owner != ""
            ca = compile_call_args(nid)
            rc = compile_expr_gc_rooted(recv)
            cmp_call = "sp_" + cmp_owner + "__cmp(" + rc + ", " + ca + ")"
            if mname == "<"
              return "(" + cmp_call + " < 0)"
            end
            if mname == ">"
              return "(" + cmp_call + " > 0)"
            end
            if mname == "<="
              return "(" + cmp_call + " <= 0)"
            end
            if mname == ">="
              return "(" + cmp_call + " >= 0)"
            end
            if mname == "=="
              return "(" + cmp_call + " == 0)"
            end
          end
        end
      end
    end
    ""
  end

  def compile_dot_call_expr(nid, recv)
    if recv >= 0
      if @nd_type[recv] == "LocalVariableReadNode"
        rname = @nd_name[recv]
        # Check method references
        ri = 0
        while ri < @method_ref_vars.length
          if @method_ref_vars[ri] == rname
            ref_mname = @method_ref_names[ri]
            mi = find_method_idx(ref_mname)
            if mi >= 0
              return "sp_" + sanitize_name(ref_mname) + "(" + compile_call_args(nid) + ")"
            end
          end
          ri = ri + 1
        end
        # Check if it's a proc variable
        vt = find_var_type(rname)
        if vt == "proc"
          # Pack the call-site args into a stack-allocated mrb_int array
          # (C99 compound literal) so a single `sp_proc_call(p, args)`
          # helper covers any arity. The proc fn unpacks args[i] into
          # named locals at function entry. compile_call_args returns
          # "" for zero args; pad with "0" so the array has at least
          # one slot (the proc fn's `_unused` fallback expects args[0]
          # to be addressable).
          ca = compile_call_args(nid)
          if ca == ""
            ca = "0"
          end
          return "sp_proc_call(lv_" + rname + ", (mrb_int[]){" + ca + "})"
        end
      end
    end
    ""
  end

  def compile_bigint_arg(nid)
    args_id = @nd_arguments[nid]
    if args_id >= 0
      arg_ids = get_args(args_id)
      if arg_ids.length > 0
        at = infer_type(arg_ids[0])
        val = compile_expr(arg_ids[0])
        if at == "bigint"
          return val
        end
        return "sp_bigint_new_int(" + val + ")"
      end
    end
    "sp_bigint_new_int(0)"
  end

  # Collect flattened parts of a string concat chain: a + b + c → [a, b, c]
  # Returns compiled expression strings. Only flattens up to 4 parts.
  def collect_concat_chain(nid)
    parts = "".split(",")
    collect_concat_parts(nid, parts)
    parts
  end

  def collect_concat_parts(nid, parts)
    if parts.length >= 12
      parts.push(compile_expr(nid))
      return
    end
    if @nd_type[nid] == "CallNode" && @nd_name[nid] == "+"
      recv = @nd_receiver[nid]
      if recv >= 0 && infer_type(recv) == "string"
        collect_concat_parts(recv, parts)
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            at = infer_type(aargs[0])
            if at == "string"
              parts.push(compile_expr(aargs[0]))
            elsif at == "int"
              parts.push("sp_int_to_s(" + compile_expr(aargs[0]) + ")")
            elsif at == "float"
              parts.push("sp_float_to_s(" + compile_expr(aargs[0]) + ")")
            else
              parts.push(compile_expr(aargs[0]))
            end
            return
          end
        end
      end
    end
    parts.push(compile_expr(nid))
  end

  def compile_operator_expr(nid, mname, recv)
    # Bigint operators
    lt = infer_type(recv)
    if lt != "bigint"
      # Check if argument is bigint
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length > 0 && infer_type(aargs[0]) == "bigint"
          lt = "bigint"
        end
      end
    end
    if lt == "bigint"
      rc_raw = compile_expr(recv)
      rc = infer_type(recv) == "bigint" ? rc_raw : "sp_bigint_new_int(" + rc_raw + ")"
      arg = compile_bigint_arg(nid)
      if mname == "+"
        return "sp_bigint_add(" + rc + ", " + arg + ")"
      end
      if mname == "-"
        return "sp_bigint_sub(" + rc + ", " + arg + ")"
      end
      if mname == "*"
        return "sp_bigint_mul(" + rc + ", " + arg + ")"
      end
      if mname == "/"
        return "sp_bigint_div(" + rc + ", " + arg + ")"
      end
      if mname == "%"
        return "sp_bigint_mod(" + rc + ", " + arg + ")"
      end
      if mname == "**" || mname == "pow"
        return "sp_bigint_pow(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == ">"
        return "(sp_bigint_cmp(" + rc + ", " + arg + ") > 0)"
      end
      if mname == "<"
        return "(sp_bigint_cmp(" + rc + ", " + arg + ") < 0)"
      end
      if mname == ">="
        return "(sp_bigint_cmp(" + rc + ", " + arg + ") >= 0)"
      end
      if mname == "<="
        return "(sp_bigint_cmp(" + rc + ", " + arg + ") <= 0)"
      end
      if mname == "=="
        return "(sp_bigint_cmp(" + rc + ", " + arg + ") == 0)"
      end
      if mname == "!="
        return "(sp_bigint_cmp(" + rc + ", " + arg + ") != 0)"
      end
    end
    # Operators
    if mname == "**" || mname == "pow"
      lt = infer_type(recv)
      if lt == "int"
        return "((mrb_int)pow((double)" + compile_expr(recv) + ", (double)" + compile_arg0(nid) + "))"
      end
      return "pow(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
    end
    if mname == "+"
      lt = infer_type(recv)
      if lt == "mutable_str"
        return "sp_str_concat(" + compile_expr(recv) + "->data, " + compile_arg0(nid) + ")"
      end
      if lt == "string"
        # Flatten chained string concat: a + b + c → sp_str_concat3(a,b,c)
        parts = collect_concat_chain(nid)
        if parts.length == 3
          return "sp_str_concat3(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ")"
        end
        if parts.length == 4
          return "sp_str_concat4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")"
        end
        if parts.length >= 5
          # Variable-length: single malloc for N parts via sp_str_concat_arr.
          # Hoist each part into a rooted temp first — the compound-literal
          # initializer order is unspecified, and any part that is a fresh
          # GC string would otherwise sit unrooted on the C stack while
          # later parts evaluate (and may trigger sp_gc_collect).
          # @needs_gc is set in scan_features for any string `+`, ensuring
          # SP_GC_SAVE() is in the function header before we emit roots here.
          tnames = "".split(",")
          k = 0
          while k < parts.length
            t = new_temp
            emit("  const char * " + t + " = " + parts[k] + ";")
            emit("  SP_GC_ROOT(" + t + ");")
            tnames.push(t)
            k = k + 1
          end
          arr = "(const char *const[]){"
          k = 0
          while k < tnames.length
            if k > 0
              arr = arr + ", "
            end
            arr = arr + tnames[k]
            k = k + 1
          end
          arr = arr + "}"
          return "sp_str_concat_arr(" + arr + ", " + tnames.length.to_s + ")"
        end
        return "sp_str_concat(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
      end
      if lt == "poly"
        @needs_rb_value = 1
        return "sp_poly_add(" + compile_expr(recv) + ", " + box_expr_to_poly(@nd_arguments[nid] >= 0 ? get_args(@nd_arguments[nid])[0] : -1) + ")"
      end
      if is_array_type(lt) == 1
        rc = compile_expr_gc_rooted(recv)
        arg = compile_arg0(nid)
        pfx = array_c_prefix(lt)
        tmp = new_temp
        itmp = new_temp
        emit("  " + c_type(lt) + tmp + " = sp_" + pfx + "_dup(" + rc + ");")
        emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < sp_" + pfx + "_length(" + arg + "); " + itmp + "++)")
        emit("    sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + arg + ", " + itmp + "));")
        return tmp
      end
      return "(" + compile_expr(recv) + " + " + compile_arg0(nid) + ")"
    end
    if mname == "-"
      lt = infer_type(recv)
      args_id = @nd_arguments[nid]
      if args_id < 0
        return "(-" + compile_expr(recv) + ")"
      end
      if lt == "poly"
        @needs_rb_value = 1
        return "sp_poly_sub(" + compile_expr(recv) + ", " + box_expr_to_poly(get_args(args_id)[0]) + ")"
      end
      return "(" + compile_expr(recv) + " - " + compile_arg0(nid) + ")"
    end
    if mname == "*"
      lt = infer_type(recv)
      if lt == "string"
        return "sp_str_repeat(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
      end
      if lt == "poly"
        @needs_rb_value = 1
        return "sp_poly_mul(" + compile_expr(recv) + ", " + box_expr_to_poly(get_args(@nd_arguments[nid])[0]) + ")"
      end
      if is_array_type(lt) == 1
        # All array kinds expose `_new` / `_length` / `_get` / `_push` with
        # the same shape, so the repeat loop is a single template
        # parameterized by the C prefix.
        mark_array_runtime_needs(lt)
        @needs_gc = 1
        pfx = array_c_prefix(lt)
        tmp = new_temp
        src = new_temp
        cnt = new_temp
        emit("  sp_" + pfx + " *" + src + " = " + compile_expr(recv) + ";")
        emit("  mrb_int " + cnt + " = " + compile_arg0(nid) + ";")
        emit("  sp_" + pfx + " *" + tmp + " = sp_" + pfx + "_new();")
        emit("  { mrb_int _mi; mrb_int _sl = sp_" + pfx + "_length(" + src + "); for (_mi = 0; _mi < " + cnt + "; _mi++) { mrb_int _mj; for (_mj = 0; _mj < _sl; _mj++) sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + src + ", _mj)); } }")
        return tmp
      end
      return "(" + compile_expr(recv) + " * " + compile_arg0(nid) + ")"
    end
    if mname == "/"
      lt = infer_type(recv)
      if lt == "float"
        return "(" + compile_expr(recv) + " / " + compile_arg0(nid) + ")"
      end
      # Check RHS for float
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length > 0
          rt = infer_type(aargs.first)
          if rt == "float"
            return "((mrb_float)" + compile_expr(recv) + " / " + compile_arg0(nid) + ")"
          end
        end
      end
      return "sp_idiv(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
    end
    if mname == "%"
      lt = infer_type(recv)
      if lt == "string" || lt == "mutable_str"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            rt = infer_type(aargs[0])
            if rt == "str_array"
              recv_c = compile_expr(recv)
              if lt == "mutable_str"
                recv_c = recv_c + "->data"
              end
              return "sp_str_format_strarr(" + recv_c + ", " + compile_expr(aargs[0]) + ")"
            end
          end
        end
      end
      if lt == "string"
        # Ruby's `"fmt" % val` — single-value form. Arg is cast to match
        # the conversion: (double) for %f, (long long) for integer
        # conversions. Ruby's `%d` is mapped to C's `%lld` when the
        # format is a string literal — otherwise pass through.
        arg0 = get_args(@nd_arguments[nid])[0]
        at = infer_type(arg0)
        fmt_c = compile_expr(recv)
        # Literal-format optimization: rewrite %d → %lld at compile time
        # (done byte-by-byte to avoid pulling in the regex engine — the
        # self-hosted bootstrap step links without libspinel_rt.a).
        if @nd_type[recv] == "StringNode"
          lit = @nd_unescaped[recv]
          if lit == ""
            lit = @nd_content[recv]
          end
          rewritten = ""
          fi = 0
          while fi < lit.length
            if lit[fi] == "%"
              # Copy "%" + any flags/width + final spec.
              rewritten = rewritten + "%"
              fi = fi + 1
              while fi < lit.length
                c = lit[fi]
                if c == "-" || c == "+" || c == " " || c == "#" || c == "0" || (c >= "1" && c <= "9") || c == "."
                  rewritten = rewritten + c
                  fi = fi + 1
                else
                  break
                end
              end
              if fi < lit.length
                c = lit[fi]
                if c == "d" || c == "i"
                  rewritten = rewritten + "lld"
                else
                  rewritten = rewritten + c
                end
                fi = fi + 1
              end
            else
              rewritten = rewritten + lit[fi]
              fi = fi + 1
            end
          end
          fmt_c = c_string_literal(rewritten)
        end
        if at == "float"
          return "sp_sprintf(" + fmt_c + ", (double)" + compile_expr(arg0) + ")"
        elsif at == "string"
          return "sp_sprintf(" + fmt_c + ", " + compile_expr(arg0) + ")"
        else
          return "sp_sprintf(" + fmt_c + ", (long long)" + compile_expr(arg0) + ")"
        end
      end
      return "sp_imod(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
    end
    if mname == "<"
      lt = infer_type(recv)
      if lt == "string"
        cc = try_char_cmp(nid, "<")
        if cc != ""
          return cc
        end
        return "(strcmp(" + compile_expr(recv) + ", " + compile_arg0(nid) + ") < 0)"
      end
      return "(" + compile_expr(recv) + " < " + compile_arg0(nid) + ")"
    end
    if mname == ">"
      lt = infer_type(recv)
      if lt == "string"
        cc = try_char_cmp(nid, ">")
        if cc != ""
          return cc
        end
        return "(strcmp(" + compile_expr(recv) + ", " + compile_arg0(nid) + ") > 0)"
      end
      if lt == "poly"
        @needs_rb_value = 1
        return "sp_poly_gt(" + compile_expr(recv) + ", " + box_expr_to_poly(get_args(@nd_arguments[nid])[0]) + ")"
      end
      return "(" + compile_expr(recv) + " > " + compile_arg0(nid) + ")"
    end
    if mname == "<="
      lt = infer_type(recv)
      if lt == "string"
        cc = try_char_cmp(nid, "<=")
        if cc != ""
          return cc
        end
        return "(strcmp(" + compile_expr(recv) + ", " + compile_arg0(nid) + ") <= 0)"
      end
      return "(" + compile_expr(recv) + " <= " + compile_arg0(nid) + ")"
    end
    if mname == ">="
      lt = infer_type(recv)
      if lt == "string"
        cc = try_char_cmp(nid, ">=")
        if cc != ""
          return cc
        end
        return "(strcmp(" + compile_expr(recv) + ", " + compile_arg0(nid) + ") >= 0)"
      end
      return "(" + compile_expr(recv) + " >= " + compile_arg0(nid) + ")"
    end
    if mname == "=~"
      # str =~ /pattern/ → sp_re_match(pat, str)
      rc = compile_expr_gc_rooted(recv)
      re_args_id = @nd_arguments[nid]
      if re_args_id >= 0
        argl = get_args(re_args_id)
        if argl.length > 0
          ridx = find_regexp_index(argl[0])
          if ridx >= 0
            return "sp_re_match(sp_re_pat_" + ridx.to_s + ", " + rc + ")"
          end
        end
      end
      return "(-1)"
    end
    if mname == "=="
      return compile_eq(nid, "==")
    end
    if mname == "!="
      return compile_eq(nid, "!=")
    end
    if mname == "!"
      return "(!" + compile_expr(recv) + ")"
    end
    if mname == "between?"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length >= 2
          rc = compile_expr_gc_rooted(recv)
          lo = compile_expr(aargs[0])
          hi = compile_expr(aargs[1])
          lt = infer_type(recv)
          if lt == "string"
            return "(strcmp(" + rc + ", " + lo + ") >= 0 && strcmp(" + rc + ", " + hi + ") <= 0)"
          end
          return "(" + rc + " >= " + lo + " && " + rc + " <= " + hi + ")"
        end
      end
    end
    if mname == "<<"
      lt = infer_type(recv)
      if lt == "mutable_str"
        @needs_mutable_str = 1
        rc = compile_expr_gc_rooted(recv)
        val = compile_arg0(nid)
        return "(sp_String_append(" + rc + ", " + val + "), " + rc + ")"
      end
      if lt == "string"
        return "sp_str_concat(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
      end
      return "(" + compile_expr(recv) + " << " + compile_arg0(nid) + ")"
    end
    if mname == ">>"
      return "(" + compile_expr(recv) + " >> " + compile_arg0(nid) + ")"
    end
    if mname == "&"
      return "(" + compile_expr(recv) + " & " + compile_arg0(nid) + ")"
    end
    if mname == "|"
      return "(" + compile_expr(recv) + " | " + compile_arg0(nid) + ")"
    end
    if mname == "^"
      return "(" + compile_expr(recv) + " ^ " + compile_arg0(nid) + ")"
    end
    if mname == "~"
      return "(~" + compile_expr(recv) + ")"
    end
    if mname == "-@"
      rt = infer_type(recv)
      if rt == "float"
        return "(-" + compile_expr(recv) + ")"
      end
      return "(-" + compile_expr(recv) + ")"
    end
    ""
  end

  def compile_constructor_expr(nid, recv)
    cname = constructor_class_name(recv)
    if cname != ""
      if cname == "Proc"
        if @nd_block[nid] >= 0
          return compile_proc_literal(nid)
        end
      end
      if cname == "Array"
        @needs_gc = 1
        args_id = @nd_arguments[nid]
        # Array.new(n) { |i| ... } -- IntArray-only fast path. We don't
        # try to introspect the block body to pick a typed container
        # (calling infer_type from this dispatch perturbs the bootstrap;
        # see bug-11 commit). Float/String collectors must be built via
        # explicit `[]` + `N.times { ... << }` instead.
        if args_id >= 0 && @nd_block[nid] >= 0
          arrnew_aargs = get_args(args_id)
          if arrnew_aargs.length >= 1
            arrnew_blk = @nd_block[nid]
            arrnew_body = @nd_body[arrnew_blk]
            arrnew_count = compile_expr(arrnew_aargs.first)
            arrnew_bp = get_block_param(nid, 0)
            arrnew_tmp = new_temp
            arrnew_iv = new_temp
            @needs_int_array = 1
            emit("  sp_IntArray *" + arrnew_tmp + " = sp_IntArray_new();")
            # Root the new array before running the block body — pushing
            # poly/string values inside the loop can trigger a GC cycle
            # that would otherwise sweep the local pointer.
            emit("  SP_GC_ROOT(" + arrnew_tmp + ");")
            emit("  for (mrb_int " + arrnew_iv + " = 0; " + arrnew_iv + " < " + arrnew_count + "; " + arrnew_iv + "++) {")
            @indent = @indent + 1
            if arrnew_bp != ""
              emit("  lv_" + arrnew_bp + " = " + arrnew_iv + ";")
            end
            if arrnew_body >= 0
              arrnew_stmts2 = get_stmts(arrnew_body)
              if arrnew_stmts2.length > 0
                arrnew_k = 0
                while arrnew_k < arrnew_stmts2.length - 1
                  compile_stmt(arrnew_stmts2[arrnew_k])
                  arrnew_k = arrnew_k + 1
                end
                arrnew_lastv = compile_expr(arrnew_stmts2.last)
                emit("  sp_IntArray_push(" + arrnew_tmp + ", " + arrnew_lastv + ");")
              end
            end
            @indent = @indent - 1
            emit("  }")
            return arrnew_tmp
          end
        end
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length >= 2
            # Array.new(n, val) - check fill value type
            vt = infer_type(aargs[1])
            if vt == "float"
              @needs_float_array = 1
              tmp = new_temp
              emit("  sp_FloatArray *" + tmp + " = sp_FloatArray_new();")
              emit("  { mrb_int _n = " + compile_expr(aargs.first) + "; mrb_float _v = " + compile_expr(aargs[1]) + "; for (mrb_int _i = 0; _i < _n; _i++) sp_FloatArray_push(" + tmp + ", _v); }")
              return tmp
            end
            if vt == "string"
              @needs_str_array = 1
              tmp = new_temp
              emit("  sp_StrArray *" + tmp + " = sp_StrArray_new();")
              emit("  { mrb_int _n = " + compile_expr(aargs.first) + "; const char *_v = " + compile_expr(aargs[1]) + "; for (mrb_int _i = 0; _i < _n; _i++) sp_StrArray_push(" + tmp + ", _v); }")
              return tmp
            end
            # Pointer-type fills (objects, other arrays) need a typed PtrArray
            # so the GC scans the elements. Without this they'd be pushed
            # into an int_array and silently swept.
            if type_is_pointer(vt) == 1
              @needs_gc = 1
              tmp = new_temp
              emit("  sp_PtrArray *" + tmp + " = sp_PtrArray_new();")
              emit("  { mrb_int _n = " + compile_expr(aargs.first) + "; void *_v = (void *)(" + compile_expr(aargs[1]) + "); for (mrb_int _i = 0; _i < _n; _i++) sp_PtrArray_push(" + tmp + ", _v); }")
              return tmp
            end
            @needs_int_array = 1
            tmp = new_temp
            emit("  sp_IntArray *" + tmp + " = sp_IntArray_new();")
            emit("  { mrb_int _n = " + compile_expr(aargs.first) + "; mrb_int _v = " + compile_expr(aargs[1]) + "; for (mrb_int _i = 0; _i < _n; _i++) sp_IntArray_push(" + tmp + ", _v); }")
            return tmp
          end
        end
        @needs_int_array = 1
        return "sp_IntArray_new()"
      end
      if cname == "Hash"
        @needs_str_int_hash = 1
        @needs_gc = 1
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length >= 1
            # Hash.new(default_val) - check type
            dt = infer_type(aargs.first)
            if dt == "string"
              @needs_str_str_hash = 1
              return "sp_StrStrHash_new()"
            end
            # Default is int - for now just return normal hash
            # Default value is handled by the get function
            return "sp_StrIntHash_new()"
          end
        end
        return "sp_StrIntHash_new()"
      end
      if cname == "StringIO"
        @needs_stringio = 1
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length >= 1
            return "sp_StringIO_new_s(" + compile_expr(aargs.first) + ")"
          end
        end
        return "sp_StringIO_new()"
      end
      # `Object.new` — a sentinel allocation. Each call returns a fresh
      # GC-managed pointer so identity comparisons (`==` / `equal?`)
      # behave as in Ruby (distinct instances are not equal).
      if cname == "Object"
        @needs_gc = 1
        return "sp_Object_new()"
      end
      ci = find_class_idx(cname)
      if ci >= 0
        return "sp_" + cname + "_new(" + compile_constructor_args(ci, nid) + ")"
      end
    end
    ""
  end

  def compile_stringio_method_expr(nid, mname, rc)
    if mname == "string"
      return "sp_StringIO_string(" + rc + ")"
    end
    if mname == "pos" || mname == "tell"
      return "sp_StringIO_pos(" + rc + ")"
    end
    if mname == "size" || mname == "length"
      return "sp_StringIO_size(" + rc + ")"
    end
    if mname == "write"
      return "sp_StringIO_write(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "read"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length >= 1
          return "sp_StringIO_read_n(" + rc + ", " + compile_expr(aargs.first) + ")"
        end
      end
      return "sp_StringIO_read(" + rc + ")"
    end
    if mname == "gets"
      return "sp_StringIO_gets(" + rc + ")"
    end
    if mname == "getc"
      return "sp_StringIO_getc(" + rc + ")"
    end
    if mname == "getbyte"
      return "sp_StringIO_getbyte(" + rc + ")"
    end
    if mname == "puts"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length >= 1
          emit("  sp_StringIO_puts(" + rc + ", " + compile_expr(aargs.first) + ");")
          return "0"
        end
      end
      emit("  sp_StringIO_puts_empty(" + rc + ");")
      return "0"
    end
    if mname == "print"
      emit("  sp_StringIO_print(" + rc + ", " + compile_arg0(nid) + ");")
      return "0"
    end
    if mname == "putc"
      return "sp_StringIO_putc(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "rewind"
      emit("  sp_StringIO_rewind(" + rc + ");")
      return "0"
    end
    if mname == "seek"
      emit("  sp_StringIO_seek(" + rc + ", " + compile_arg0(nid) + ");")
      return "0"
    end
    if mname == "truncate"
      emit("  sp_StringIO_truncate(" + rc + ", " + compile_arg0(nid) + ");")
      return "0"
    end
    if mname == "close"
      emit("  sp_StringIO_close(" + rc + ");")
      return "0"
    end
    if mname == "eof?"
      return "sp_StringIO_eof_p(" + rc + ")"
    end
    if mname == "closed?"
      return "sp_StringIO_closed_p(" + rc + ")"
    end
    if mname == "flush"
      return "sp_StringIO_flush(" + rc + ")"
    end
    if mname == "sync"
      return "sp_StringIO_sync(" + rc + ")"
    end
    if mname == "isatty"
      return "sp_StringIO_isatty(" + rc + ")"
    end
    ""
  end

  def compile_string_method_expr(nid, mname, rc)
    if mname == "length"
      # Only use hoisted length if the receiver matches (otherwise we'd
      # return the wrong string's length).
      if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
        return @hoisted_strlen_var
      end
      return "sp_str_length(" + rc + ")"
    end
    if mname == "to_i"
      return "((mrb_int)atoll(" + rc + "))"
    end
    if mname == "to_f"
      return "atof(" + rc + ")"
    end
    if mname == "inspect"
      return "sp_str_inspect(" + rc + ")"
    end
    if mname == "upcase"
      return "sp_str_upcase(" + rc + ")"
    end
    if mname == "downcase"
      return "sp_str_downcase(" + rc + ")"
    end
    if mname == "swapcase"
      return "sp_str_swapcase(" + rc + ")"
    end
    if mname == "delete_prefix"
      return "sp_str_delete_prefix(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "delete_suffix"
      return "sp_str_delete_suffix(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "succ" || mname == "next"
      return "sp_str_succ(" + rc + ")"
    end
    if mname == "sum"
      tmp = new_temp
      itmp = new_temp
      emit("  mrb_int " + tmp + " = 0;")
      emit("  for (mrb_int " + itmp + " = 0; " + rc + "[" + itmp + "]; " + itmp + "++) " + tmp + " += (unsigned char)" + rc + "[" + itmp + "];")
      return tmp
    end
    if mname == "eql?"
      return "(strcmp(" + rc + ", " + compile_arg0(nid) + ") == 0)"
    end
    if mname == "partition"
      tt = "tuple:string,string,string"
      register_tuple_type(tt)
      @needs_gc = 1
      tname = tuple_c_name(tt)
      sep = compile_arg0(nid)
      tmp = new_temp
      emit("  " + tname + " *" + tmp + " = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
      emit("  { const char *_p = strstr(" + rc + ", " + sep + ");")
      emit("    if (_p) { " + tmp + "->_0 = sp_str_substr(" + rc + ", 0, _p - " + rc + "); " + tmp + "->_1 = " + sep + "; " + tmp + "->_2 = sp_str_substr(" + rc + ", _p - " + rc + " + strlen(" + sep + "), strlen(_p) - strlen(" + sep + ")); }")
      emit("    else { " + tmp + "->_0 = " + rc + "; " + tmp + "->_1 = \"\"; " + tmp + "->_2 = \"\"; } }")
      return tmp
    end
    if mname == "rpartition"
      tt = "tuple:string,string,string"
      register_tuple_type(tt)
      @needs_gc = 1
      tname = tuple_c_name(tt)
      sep = compile_arg0(nid)
      tmp = new_temp
      emit("  " + tname + " *" + tmp + " = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
      emit("  { size_t _sl = strlen(" + rc + "), _pl = strlen(" + sep + "); const char *_last = NULL;")
      emit("    for (const char *_p = " + rc + "; (_p = strstr(_p, " + sep + ")); _p += _pl) _last = _p;")
      emit("    if (_last) { " + tmp + "->_0 = sp_str_substr(" + rc + ", 0, _last - " + rc + "); " + tmp + "->_1 = " + sep + "; " + tmp + "->_2 = sp_str_substr(" + rc + ", _last - " + rc + " + _pl, _sl - (_last - " + rc + ") - _pl); }")
      emit("    else { " + tmp + "->_0 = \"\"; " + tmp + "->_1 = \"\"; " + tmp + "->_2 = " + rc + "; } }")
      return tmp
    end
    if mname == "hash"
      return "(mrb_int)sp_str_hash(" + rc + ")"
    end
    if mname == "encode" || mname == "force_encoding" || mname == "b"
      return rc
    end
    if mname == "strip"
      return "sp_str_strip(" + rc + ")"
    end
    if mname == "chomp"
      return "sp_str_chomp(" + rc + ")"
    end
    if mname == "include?"
      return "sp_str_include(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "start_with?"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length > 1
          parts = "".split(",")
          k = 0
          while k < aargs.length
            parts.push("sp_str_start_with(" + rc + ", " + compile_expr(aargs[k]) + ")")
            k = k + 1
          end
          return "(" + parts.join(" || ") + ")"
        end
      end
      return "sp_str_start_with(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "end_with?"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length > 1
          parts = "".split(",")
          k = 0
          while k < aargs.length
            parts.push("sp_str_end_with(" + rc + ", " + compile_expr(aargs[k]) + ")")
            k = k + 1
          end
          return "(" + parts.join(" || ") + ")"
        end
      end
      return "sp_str_end_with(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "split"
      @needs_str_array = 1
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length > 0
          ridx = find_regexp_index(a[0])
          if ridx >= 0
            return "sp_re_split(sp_re_pat_" + ridx.to_s + ", " + rc + ")"
          end
          # Peephole: literal "".split(literal) is the empty-StrArray idiom;
          # skip the strlen+sep scan and emit a direct allocator call.
          recv = @nd_receiver[nid]
          if recv >= 0 && @nd_type[recv] == "StringNode" && @nd_content[recv] == "" && @nd_type[a[0]] == "StringNode"
            return "sp_StrArray_new()"
          end
        end
      end
      return "sp_str_split(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "lines"
      @needs_str_array = 1
      return "sp_str_split(" + rc + ", \"\\n\")"
    end
    if mname == "scan"
      if @nd_block[nid] < 0
        args_id = @nd_arguments[nid]
        if args_id >= 0
          argl = get_args(args_id)
          if argl.length > 0
            ridx = find_regexp_index(argl[0])
            if ridx >= 0
              @needs_str_array = 1
              @needs_regexp = 1
              return "sp_re_scan(sp_re_pat_" + ridx.to_s + ", " + rc + ")"
            end
          end
        end
      end
    end
    if mname == "match?"
      re_args_id = @nd_arguments[nid]
      if re_args_id >= 0
        argl = get_args(re_args_id)
        if argl.length > 0
          ridx = find_regexp_index(argl[0])
          if ridx >= 0
            return "sp_re_match_p(sp_re_pat_" + ridx.to_s + ", " + rc + ")"
          end
        end
      end
      return "sp_str_include(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "gsub"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          ridx = find_regexp_index(a[0])
          if ridx >= 0
            return "sp_re_gsub(sp_re_pat_" + ridx.to_s + ", " + rc + ", " + compile_expr(a[1]) + ")"
          end
          return "sp_str_gsub(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return rc
    end
    if mname == "sub"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          ridx = find_regexp_index(a[0])
          if ridx >= 0
            return "sp_re_sub(sp_re_pat_" + ridx.to_s + ", " + rc + ", " + compile_expr(a[1]) + ")"
          end
          return "sp_str_sub(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return rc
    end
    if mname == "index"
      return "sp_str_index(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "rindex"
      return "sp_str_rindex(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "tr"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "sp_str_tr(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return rc
    end
    if mname == "ljust"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "sp_str_ljust2(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return "sp_str_ljust(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "rjust"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "sp_str_rjust2(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return "sp_str_rjust(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "[]"
      args_id = @nd_arguments[nid]
      # Use length-aware variant if strlen of this receiver is hoisted
      use_len = (@hoisted_strlen_var != "" && @hoisted_strlen_recv == rc)
      fn = use_len ? "sp_str_sub_range_len" : "sp_str_sub_range"
      lprefix = use_len ? (rc + ", " + @hoisted_strlen_var) : rc
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 1
          if @nd_type[a[0]] == "RangeNode"
            # s[1..3] inclusive, s[1...3] exclusive
            left = compile_expr(@nd_left[a[0]])
            right = compile_expr(@nd_right[a[0]])
            adj = range_excl_end(a[0]) == 1 ? "" : " + 1"
            return fn + "(" + lprefix + ", " + left + ", " + right + " - " + left + adj + ")"
          end
          if a.length >= 2
            # s[0, 2]
            return fn + "(" + lprefix + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
          end
        end
      end
      return fn + "(" + lprefix + ", " + compile_arg0(nid) + ", 1)"
    end
    if mname == "reverse"
      return "sp_str_reverse(" + rc + ")"
    end
    if mname == "freeze"
      return rc
    end
    if mname == "frozen?"
      return "TRUE"
    end
    if mname == "to_sym" || mname == "intern"
      return "sp_sym_intern(" + rc + ")"
    end
    if mname == "ord"
      return "sp_str_ord(" + rc + ")"
    end
    if mname == "sub"
      args_id = @nd_arguments[nid]
      arg1 = "\"\""
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          arg1 = compile_expr(a[1])
        end
      end
      return "sp_str_sub(" + rc + ", " + compile_arg0(nid) + ", " + arg1 + ")"
    end
    if mname == "capitalize"
      return "sp_str_capitalize(" + rc + ")"
    end
    if mname == "count"
      return "sp_str_count(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "*"
      return "sp_str_repeat(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "empty?"
      return "(strlen(" + rc + ") == 0)"
    end
    if mname == "chars"
      @needs_str_array = 1
      @needs_gc = 1
      return "sp_str_split(" + rc + ", \"\")"
    end
    if mname == "bytes"
      @needs_int_array = 1
      @needs_gc = 1
      return "sp_str_bytes(" + rc + ")"
    end
    if mname == "hex"
      return "((mrb_int)strtoll(" + rc + ", NULL, 16))"
    end
    if mname == "oct"
      return "((mrb_int)strtoll(" + rc + ", NULL, 8))"
    end
    if mname == "tr"
      args_id = @nd_arguments[nid]
      arg1 = "\"\""
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          arg1 = compile_expr(a[1])
        end
      end
      return "sp_str_tr(" + rc + ", " + compile_arg0(nid) + ", " + arg1 + ")"
    end
    if mname == "delete"
      return "sp_str_delete(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "squeeze"
      return "sp_str_squeeze(" + rc + ")"
    end
    if mname == "size"
      if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
        return @hoisted_strlen_var
      end
      return "sp_str_length(" + rc + ")"
    end
    if mname == "slice"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "sp_str_sub_range(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return "sp_str_sub_range(" + rc + ", " + compile_arg0(nid) + ", 1)"
    end
    if mname == "center"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "sp_str_center2(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
      return "sp_str_center(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "lstrip"
      return "sp_str_lstrip(" + rc + ")"
    end
    if mname == "rstrip"
      return "sp_str_rstrip(" + rc + ")"
    end
    if mname == "dup"
      return "sp_str_dup(" + rc + ")"
    end
    if mname == "getbyte"
      return "((mrb_int)(unsigned char)(" + rc + ")[" + compile_arg0(nid) + "])"
    end
    if mname == "setbyte"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "(((char*)" + rc + ")[" + compile_expr(a[0]) + "] = (char)" + compile_expr(a[1]) + ", 0)"
        end
      end
      return "0"
    end
    if mname == "bytesize"
      return "(mrb_int)strlen(" + rc + ")"
    end
    if mname == "to_s"
      return rc
    end
    ""
  end

  def compile_range_method_expr(nid, mname, rc)
    if mname == "first"
      return rc + ".first"
    end
    if mname == "last"
      return rc + ".last"
    end
    if mname == "include?"
      tmp = new_temp
      emit("  sp_Range " + tmp + " = " + rc + ";")
      return "(" + compile_arg0(nid) + " >= " + tmp + ".first && " + compile_arg0(nid) + " <= " + tmp + ".last)"
    end
    if mname == "to_a"
      @needs_int_array = 1
      @needs_gc = 1
      # Honour `...` exclusive Range when the receiver is a literal
      # RangeNode (or wrapped in parens). For non-literal Range values
      # held in sp_Range structs, exclude_end is not tracked at runtime
      # and the inclusive form is used.
      recv = @nd_receiver[nid]
      range_nid = -1
      if recv >= 0 && @nd_type[recv] == "RangeNode"
        range_nid = recv
      end
      if recv >= 0 && @nd_type[recv] == "ParenthesesNode"
        pb = @nd_body[recv]
        if pb >= 0
          ps = get_stmts(pb)
          if ps.length > 0 && @nd_type[ps.first] == "RangeNode"
            range_nid = ps.first
          end
        end
      end
      if range_nid >= 0
        rright = compile_expr(@nd_right[range_nid])
        if range_excl_end(range_nid) == 1
          rright = "(" + rright + ") - 1"
        end
        return "sp_IntArray_from_range(" + compile_expr(@nd_left[range_nid]) + ", " + rright + ")"
      end
      return "sp_IntArray_from_range(" + rc + ".first, " + rc + ".last)"
    end
    if mname == "length"
      return "(" + rc + ".last - " + rc + ".first + 1)"
    end
    if mname == "size"
      return "(" + rc + ".last - " + rc + ".first + 1)"
    end
    ""
  end

  # Symbol methods. rc is a sp_sym expression.
  def compile_symbol_method_expr(nid, mname, rc)
    if mname == "to_s" || mname == "id2name" || mname == "name"
      return "sp_sym_to_s(" + rc + ")"
    end
    if mname == "to_sym" || mname == "intern"
      return rc
    end
    if mname == "inspect"
      return "sp_str_concat(\":\", sp_sym_to_s(" + rc + "))"
    end
    if mname == "length" || mname == "size"
      return "((mrb_int)strlen(sp_sym_to_s(" + rc + ")))"
    end
    if mname == "empty?"
      return "(sp_sym_to_s(" + rc + ")[0] == 0)"
    end
    if mname == "hash"
      return "((mrb_int)" + rc + ")"
    end
    if mname == "<=>"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length >= 1
          at = infer_type(aargs[0])
          if at == "symbol"
            other = compile_expr(aargs[0])
            # Lexical compare on symbol names (Ruby semantics)
            cmp = "strcmp(sp_sym_to_s(" + rc + "), sp_sym_to_s(" + other + "))"
            return "((" + cmp + ") < 0 ? (mrb_int)-1 : ((" + cmp + ") > 0 ? (mrb_int)1 : (mrb_int)0))"
          end
        end
      end
      return "0"
    end
    if mname == "==" || mname == "eql?"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length >= 1
          at = infer_type(aargs[0])
          if at == "symbol"
            return "(" + rc + " == " + compile_expr(aargs[0]) + ")"
          end
          # Symbol != anything-non-symbol in Ruby
          return "FALSE"
        end
      end
    end
    if mname == "!="
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aargs = get_args(args_id)
        if aargs.length >= 1
          at = infer_type(aargs[0])
          if at == "symbol"
            return "(" + rc + " != " + compile_expr(aargs[0]) + ")"
          end
          return "TRUE"
        end
      end
    end
    ""
  end

  def compile_int_method_expr(nid, mname, rc)
    if mname == "to_s"
      if @nd_arguments[nid] >= 0
        aargs = get_args(@nd_arguments[nid])
        if aargs.length > 0
          return "sp_int_to_s_base(" + rc + ", " + compile_expr(aargs[0]) + ")"
        end
      end
      return "sp_int_to_s(" + rc + ")"
    end
    if mname == "inspect"
      return "sp_int_to_s(" + rc + ")"
    end
    if mname == "digits"
      @needs_int_array = 1
      @needs_gc = 1
      base = "10"
      if @nd_arguments[nid] >= 0
        aargs = get_args(@nd_arguments[nid])
        if aargs.length > 0
          base = compile_expr(aargs[0])
        end
      end
      return "sp_int_digits(" + rc + ", " + base + ")"
    end
    if mname == "bit_length"
      # Number of bits needed to represent the integer (excluding sign)
      return "((" + rc + ") < 0 ? (64 - __builtin_clzll((uint64_t)~(" + rc + "))) : ((" + rc + ") == 0 ? 0 : (64 - __builtin_clzll((uint64_t)(" + rc + ")))))"
    end
    if mname == "fdiv"
      return "((mrb_float)(" + rc + ") / (mrb_float)" + compile_arg0(nid) + ")"
    end
    if mname == "divmod"
      tt = "tuple:int,int"
      register_tuple_type(tt)
      @needs_gc = 1
      name = tuple_c_name(tt)
      arg = compile_arg0(nid)
      tmp = new_temp
      emit("  " + name + " *" + tmp + " = (" + name + " *)sp_gc_alloc(sizeof(" + name + "), NULL, " + tuple_scan_name(tt) + ");")
      emit("  " + tmp + "->_0 = " + rc + " / " + arg + ";")
      emit("  " + tmp + "->_1 = sp_imod(" + rc + ", " + arg + ");")
      return tmp
    end
    if mname == "to_i"
      return rc
    end
    if mname == "to_f"
      return "(mrb_float)(" + rc + ")"
    end
    if mname == "abs"
      return "((" + rc + ") < 0 ? -(" + rc + ") : (" + rc + "))"
    end
    if mname == "even?"
      return "((" + rc + ") % 2 == 0)"
    end
    if mname == "odd?"
      return "((" + rc + ") % 2 != 0)"
    end
    if mname == "zero?"
      return "((" + rc + ") == 0)"
    end
    if mname == "gcd"
      return "sp_gcd(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "clamp"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 2
          return "sp_int_clamp(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
        end
      end
    end
    if mname == "frozen?"
      return "TRUE"
    end
    if mname == "chr"
      return "sp_int_chr(" + rc + ")"
    end
    if mname == "succ" || mname == "next"
      return "((" + rc + ") + 1)"
    end
    if mname == "itself"
      return rc
    end
    ""
  end

  def compile_float_method_expr(nid, mname, rc)
    if mname == "itself"
      return rc
    end
    if mname == "to_s"
      return "sp_float_to_s(" + rc + ")"
    end
    if mname == "inspect"
      return "sp_float_inspect(" + rc + ")"
    end
    if mname == "to_i"
      return "(mrb_int)(" + rc + ")"
    end
    if mname == "ceil"
      return "(mrb_int)ceil(" + rc + ")"
    end
    if mname == "floor"
      return "(mrb_int)floor(" + rc + ")"
    end
    if mname == "round"
      return "(mrb_int)round(" + rc + ")"
    end
    if mname == "abs"
      return "fabs(" + rc + ")"
    end
    if mname == "nan?"
      return "(isnan(" + rc + ") ? TRUE : FALSE)"
    end
    if mname == "finite?"
      return "(isfinite(" + rc + ") ? TRUE : FALSE)"
    end
    if mname == "infinite?"
      return "(isinf(" + rc + ") ? (" + rc + " < 0 ? -1 : 1) : 0)"
    end
    if mname == "truncate"
      return "(mrb_int)trunc(" + rc + ")"
    end
    if mname == "fdiv"
      return "((" + rc + ") / (mrb_float)" + compile_arg0(nid) + ")"
    end
    if mname == "divmod"
      tt = "tuple:int,float"
      register_tuple_type(tt)
      @needs_gc = 1
      tname = tuple_c_name(tt)
      arg = compile_arg0(nid)
      tmp = new_temp
      emit("  " + tname + " *" + tmp + " = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
      emit("  " + tmp + "->_0 = (mrb_int)floor(" + rc + " / " + arg + ");")
      emit("  " + tmp + "->_1 = " + rc + " - " + tmp + "->_0 * " + arg + ";")
      return tmp
    end
    ""
  end

  def compile_array_method_expr(nid, mname, rc, recv_type)
    # Skip non-array types. Without this guard a user class with a
    # method whose name happens to overlap an Array method (e.g.
    # `def sample`, `def first`) would be dispatched as that Array
    # method, with `array_c_prefix` falling back to `IntArray` and
    # the receiver pointer used as if it were an `sp_IntArray *`.
    if is_array_type(recv_type) == 0
      return ""
    end
    # Array#inspect and Array#to_s (CRuby aliases them for arrays, so
    # the two share one definition via compile_inspect_for). Guard on
    # recv_type being an actual array type so scalar receivers with
    # the same method name (e.g. (poly).to_s, (int).to_s) fall through
    # to their own scalar dispatchers.
    if mname == "inspect" || mname == "to_s"
      if is_array_type(recv_type) == 1
        r = compile_inspect_for(recv_type, rc)
        if r != ""
          return r
        end
      end
    end
    # zip without block: return array of pairs/tuples
    if mname == "zip" && @nd_block[nid] < 0
      @needs_gc = 1
      pfx_recv = array_c_prefix(recv_type)
      args_id = @nd_arguments[nid]
      aargs = get_args(args_id)
      # Check if heterogeneous or multi-arg
      heterogeneous = 0
      k = 0
      while k < aargs.length
        at = infer_type(aargs[k])
        if at != recv_type
          heterogeneous = 1
        end
        k = k + 1
      end
      if aargs.length > 1
        heterogeneous = 1
      end
      # Compile all zip arguments
      arg_rcs = "".split(",")
      arg_types = "".split(",")
      k = 0
      while k < aargs.length
        arg_rcs.push(compile_expr(aargs[k]))
        arg_types.push(infer_type(aargs[k]))
        k = k + 1
      end
      tmp = new_temp
      itmp = new_temp
      pair_tmp = new_temp
      emit("  sp_PtrArray *" + tmp + " = sp_PtrArray_new();")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < sp_" + pfx_recv + "_length(" + rc + "); " + itmp + "++) {")
      if heterogeneous == 1
        # Build tuple type
        parts = "".split(",")
        parts.push(elem_type_of_array(recv_type))
        k = 0
        while k < arg_types.length
          parts.push(elem_type_of_array(arg_types[k]))
          k = k + 1
        end
        tt = "tuple:" + parts.join(",")
        register_tuple_type(tt)
        tname = tuple_c_name(tt)
        emit("    " + tname + " *" + pair_tmp + " = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
        emit("    " + pair_tmp + "->_0 = sp_" + pfx_recv + "_get(" + rc + ", " + itmp + ");")
        k = 0
        while k < arg_rcs.length
          apfx = array_c_prefix(arg_types[k])
          emit("    " + pair_tmp + "->_" + (k + 1).to_s + " = sp_" + apfx + "_get(" + arg_rcs[k] + ", " + itmp + ");")
          k = k + 1
        end
      else
        emit("    " + c_type(recv_type) + " " + pair_tmp + " = sp_" + pfx_recv + "_new();")
        emit("    sp_" + pfx_recv + "_push(" + pair_tmp + ", sp_" + pfx_recv + "_get(" + rc + ", " + itmp + "));")
        k = 0
        while k < arg_rcs.length
          emit("    sp_" + pfx_recv + "_push(" + pair_tmp + ", sp_" + pfx_recv + "_get(" + arg_rcs[k] + ", " + itmp + "));")
          k = k + 1
        end
      end
      emit("    sp_PtrArray_push(" + tmp + ", " + pair_tmp + ");")
      emit("  }")
      return tmp
    end
    # first(n) / last(n) with argument: return new array
    if mname == "first" && @nd_arguments[nid] >= 0
      aargs = get_args(@nd_arguments[nid])
      if aargs.length > 0
        pfx = array_c_prefix(recv_type)
        n = compile_expr(aargs[0])
        tmp = new_temp
        itmp = new_temp
        emit("  " + c_type(recv_type) + " " + tmp + " = sp_" + pfx + "_new();")
        emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + n + " && " + itmp + " < sp_" + pfx + "_length(" + rc + "); " + itmp + "++)")
        emit("    sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + rc + ", " + itmp + "));")
        return tmp
      end
    end
    if mname == "last" && @nd_arguments[nid] >= 0
      aargs = get_args(@nd_arguments[nid])
      if aargs.length > 0
        pfx = array_c_prefix(recv_type)
        n = compile_expr(aargs[0])
        tmp = new_temp
        itmp = new_temp
        len_tmp = new_temp
        emit("  mrb_int " + len_tmp + " = sp_" + pfx + "_length(" + rc + ");")
        emit("  " + c_type(recv_type) + " " + tmp + " = sp_" + pfx + "_new();")
        emit("  for (mrb_int " + itmp + " = (" + len_tmp + " - " + n + " < 0 ? 0 : " + len_tmp + " - " + n + "); " + itmp + " < " + len_tmp + "; " + itmp + "++)")
        emit("    sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + rc + ", " + itmp + "));")
        return tmp
      end
    end
    # Common array methods (all array types)
    if mname == "take"
      pfx = array_c_prefix(recv_type)
      n = compile_arg0(nid)
      tmp = new_temp
      itmp = new_temp
      emit("  " + c_type(recv_type) + tmp + " = sp_" + pfx + "_new();")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + n + " && " + itmp + " < sp_" + pfx + "_length(" + rc + "); " + itmp + "++)")
      emit("    sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + rc + ", " + itmp + "));")
      return tmp
    end
    if mname == "drop"
      pfx = array_c_prefix(recv_type)
      n = compile_arg0(nid)
      tmp = new_temp
      itmp = new_temp
      emit("  " + c_type(recv_type) + tmp + " = sp_" + pfx + "_new();")
      emit("  for (mrb_int " + itmp + " = " + n + "; " + itmp + " < sp_" + pfx + "_length(" + rc + "); " + itmp + "++)")
      emit("    sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + rc + ", " + itmp + "));")
      return tmp
    end
    if mname == "fill"
      pfx = array_c_prefix(recv_type)
      args_id_fill = @nd_arguments[nid]
      val = compile_arg0(nid)
      start_expr = "0"
      # Default end: current array length (matches CRuby's no-args
      # form which fills the entire existing array).
      end_expr = "sp_" + pfx + "_length(" + rc + ")"
      if args_id_fill >= 0
        aargs_fill = get_args(args_id_fill)
        if aargs_fill.length >= 3
          # arr.fill(value, start, length): negative start counts from
          # the end; if still negative after that, clamp to 0
          # (matches CRuby: `[1,2,3].fill(9, -5, 2) #=> [9, 9, 3]`).
          # end_expr = start + length lets the array grow past its
          # current length when start+length > length; sp_*_set
          # auto-grows and zero-fills gaps (matches CRuby:
          # `[1,2,3].fill(9, 5, 2) #=> [1, 2, 3, 0, 0, 9, 9]`).
          start_tmp = new_temp
          len_tmp = new_temp
          emit("  mrb_int " + start_tmp + " = " + compile_expr(aargs_fill[1]) + ";")
          emit("  mrb_int " + len_tmp + " = " + compile_expr(aargs_fill[2]) + ";")
          emit("  if (" + start_tmp + " < 0) " + start_tmp + " += sp_" + pfx + "_length(" + rc + ");")
          emit("  if (" + start_tmp + " < 0) " + start_tmp + " = 0;")
          start_expr = start_tmp
          end_expr = "(" + start_tmp + " + " + len_tmp + ")"
        elsif aargs_fill.length == 2
          # arr.fill(value, start): fills from start to end of EXISTING
          # array. If start >= length, fills nothing (does NOT grow,
          # matching CRuby: `[1,2,3].fill(9, 5) #=> [1, 2, 3]`).
          start_tmp = new_temp
          emit("  mrb_int " + start_tmp + " = " + compile_expr(aargs_fill[1]) + ";")
          emit("  if (" + start_tmp + " < 0) " + start_tmp + " += sp_" + pfx + "_length(" + rc + ");")
          emit("  if (" + start_tmp + " < 0) " + start_tmp + " = 0;")
          start_expr = start_tmp
        end
      end
      itmp = new_temp
      emit("  for (mrb_int " + itmp + " = " + start_expr + "; " + itmp + " < " + end_expr + "; " + itmp + "++)")
      emit("    sp_" + pfx + "_set(" + rc + ", " + itmp + ", " + val + ");")
      return rc
    end
    if mname == "rotate"
      pfx = array_c_prefix(recv_type)
      n = compile_arg0(nid)
      if n == "0"
        n = "1"
      end
      tmp = new_temp
      itmp = new_temp
      len_tmp = new_temp
      emit("  mrb_int " + len_tmp + " = sp_" + pfx + "_length(" + rc + ");")
      emit("  " + c_type(recv_type) + tmp + " = sp_" + pfx + "_new();")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + len_tmp + "; " + itmp + "++)")
      emit("    sp_" + pfx + "_push(" + tmp + ", sp_" + pfx + "_get(" + rc + ", ((" + itmp + " + " + n + ") % " + len_tmp + " + " + len_tmp + ") % " + len_tmp + "));")
      return tmp
    end
    if mname == "sample"
      @needs_rand = 1
      pfx = array_c_prefix(recv_type)
      return "sp_" + pfx + "_get(" + rc + ", rand() % sp_" + pfx + "_length(" + rc + "))"
    end
    if mname == "shuffle" && is_array_type(recv_type) == 1
      @needs_rand = 1
      pfx = array_c_prefix(recv_type)
      return "sp_" + pfx + "_shuffle(" + rc + ")"
    end
    if mname == "shuffle!" && is_array_type(recv_type) == 1
      @needs_rand = 1
      pfx = array_c_prefix(recv_type)
      emit("  sp_" + pfx + "_shuffle_bang(" + rc + ");")
      return rc
    end
    if mname == "any?" && @nd_block[nid] < 0
      pfx = array_c_prefix(recv_type)
      return "(sp_" + pfx + "_length(" + rc + ") > 0)"
    end
    if mname == "none?" && @nd_block[nid] < 0
      pfx = array_c_prefix(recv_type)
      return "(sp_" + pfx + "_length(" + rc + ") == 0)"
    end
    if mname == "count" && @nd_arguments[nid] >= 0 && @nd_block[nid] < 0
      # count(val) — count occurrences of a specific value
      pfx = array_c_prefix(recv_type)
      val = compile_arg0(nid)
      tmp_c = new_temp
      tmp_i = new_temp
      if recv_type == "str_array"
        emit("  mrb_int " + tmp_c + " = 0;")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_" + pfx + "_length(" + rc + "); " + tmp_i + "++)")
        emit("    if (strcmp(sp_" + pfx + "_get(" + rc + ", " + tmp_i + "), " + val + ") == 0) " + tmp_c + "++;")
      else
        emit("  mrb_int " + tmp_c + " = 0;")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_" + pfx + "_length(" + rc + "); " + tmp_i + "++)")
        emit("    if (sp_" + pfx + "_get(" + rc + ", " + tmp_i + ") == " + val + ") " + tmp_c + "++;")
      end
      return tmp_c
    end
    if (mname == "any?" || mname == "all?" || mname == "none?" || mname == "one?") && @nd_block[nid] >= 0
      return compile_array_predicate_block(nid, rc, recv_type, mname)
    end
    if (mname == "find" || mname == "detect") && @nd_block[nid] >= 0
      return compile_array_find_block(nid, rc, recv_type)
    end
    if mname == "filter_map" && @nd_block[nid] >= 0
      return compile_array_filter_map(nid, rc, recv_type)
    end
    if (mname == "sum") && @nd_block[nid] >= 0
      return compile_array_sum_block(nid, rc, recv_type)
    end
    if (mname == "count") && @nd_block[nid] >= 0
      return compile_array_count_block(nid, rc, recv_type)
    end
    if mname == "partition" && @nd_block[nid] >= 0
      pfx = array_c_prefix(recv_type)
      tt = "tuple:" + recv_type + "," + recv_type
      register_tuple_type(tt)
      @needs_gc = 1
      name = tuple_c_name(tt)
      bp1 = get_block_param(nid, 0)
      if bp1 == ""
        bp1 = "_x"
      end
      et = elem_type_of_array(recv_type)
      tmp_t = new_temp
      tmp_f = new_temp
      tmp_res = new_temp
      itmp = new_temp
      emit("  " + c_type(recv_type) + " " + tmp_t + " = sp_" + pfx + "_new();")
      emit("  " + c_type(recv_type) + " " + tmp_f + " = sp_" + pfx + "_new();")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < sp_" + pfx + "_length(" + rc + "); " + itmp + "++) {")
      emit("    " + c_type(et) + " lv_" + bp1 + " = sp_" + pfx + "_get(" + rc + ", " + itmp + ");")
      push_scope
      declare_var(bp1, et)
      blk = @nd_block[nid]
      bexpr = "0"
      if @nd_body[blk] >= 0
        bs = get_stmts(@nd_body[blk])
        if bs.length > 0
          k = 0
          while k < bs.length - 1
            compile_stmt(bs[k])
            k = k + 1
          end
          bexpr = compile_expr(bs.last)
        end
      end
      emit("    if (" + bexpr + ") sp_" + pfx + "_push(" + tmp_t + ", lv_" + bp1 + "); else sp_" + pfx + "_push(" + tmp_f + ", lv_" + bp1 + ");")
      pop_scope
      emit("  }")
      emit("  " + name + " *" + tmp_res + " = (" + name + " *)sp_gc_alloc(sizeof(" + name + "), NULL, " + tuple_scan_name(tt) + ");")
      emit("  " + tmp_res + "->_0 = " + tmp_t + ";")
      emit("  " + tmp_res + "->_1 = " + tmp_f + ";")
      return tmp_res
    end
    if mname == "minmax"
      pfx = array_c_prefix(recv_type)
      et = elem_type_of_array(recv_type)
      tt = "tuple:" + et + "," + et
      register_tuple_type(tt)
      @needs_gc = 1
      name = tuple_c_name(tt)
      tmp = new_temp
      tmp_min = new_temp
      tmp_max = new_temp
      itmp = new_temp
      emit("  " + c_type(et) + " " + tmp_min + " = sp_" + pfx + "_get(" + rc + ", 0);")
      emit("  " + c_type(et) + " " + tmp_max + " = " + tmp_min + ";")
      emit("  for (mrb_int " + itmp + " = 1; " + itmp + " < sp_" + pfx + "_length(" + rc + "); " + itmp + "++) {")
      emit("    " + c_type(et) + " _v = sp_" + pfx + "_get(" + rc + ", " + itmp + ");")
      if et == "string"
        emit("    if (strcmp(_v, " + tmp_min + ") < 0) " + tmp_min + " = _v;")
        emit("    if (strcmp(_v, " + tmp_max + ") > 0) " + tmp_max + " = _v;")
      else
        emit("    if (_v < " + tmp_min + ") " + tmp_min + " = _v;")
        emit("    if (_v > " + tmp_max + ") " + tmp_max + " = _v;")
      end
      emit("  }")
      emit("  " + name + " *" + tmp + " = (" + name + " *)sp_gc_alloc(sizeof(" + name + "), NULL, " + tuple_scan_name(tt) + ");")
      emit("  " + tmp + "->_0 = " + tmp_min + ";")
      emit("  " + tmp + "->_1 = " + tmp_max + ";")
      return tmp
    end
    if (mname == "min" || mname == "max") && @nd_block[nid] >= 0
      return compile_array_min_max_block(nid, rc, recv_type, mname)
    end
    # Array methods
    if recv_type == "int_array" || recv_type == "sym_array"
      if mname == "length" || mname == "size"
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_IntArray_length(" + rc + ")"
      end
      if mname == "[]"
        # a[range] and a[start, len] return slices; bare a[i] stays a get.
        # Mirrors compile_string_method_expr's slicing dispatch.
        args_id = @nd_arguments[nid]
        if args_id >= 0
          a = get_args(args_id)
          if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
            left = compile_expr(@nd_left[a[0]])
            right = compile_expr(@nd_right[a[0]])
            adj = range_excl_end(a[0]) == 1 ? "" : " + 1"
            return "sp_IntArray_slice(" + rc + ", " + left + ", " + right + " - " + left + adj + ")"
          end
          if a.length >= 2
            return "sp_IntArray_slice(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
          end
        end
        return "sp_IntArray_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "push"
        return "(sp_IntArray_push(" + rc + ", " + compile_arg0(nid) + "), 0)"
      end
      if mname == "pop"
        return "sp_IntArray_pop(" + rc + ")"
      end
      if mname == "shift"
        return "sp_IntArray_shift(" + rc + ")"
      end
      if mname == "empty?"
        return "sp_IntArray_empty(" + rc + ")"
      end
      if mname == "include?"
        return "sp_IntArray_include(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "index" || mname == "find_index"
        if @nd_arguments[nid] >= 0
          return "sp_IntArray_index(" + rc + ", " + compile_arg0(nid) + ")"
        end
      end
      if mname == "rindex"
        if @nd_arguments[nid] >= 0
          return "sp_IntArray_rindex(" + rc + ", " + compile_arg0(nid) + ")"
        end
      end
      if mname == "delete_at"
        return "sp_IntArray_delete_at(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "insert"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length >= 2
            return "(sp_IntArray_insert(" + rc + ", " + compile_expr(aargs[0]) + ", " + compile_expr(aargs[1]) + "), " + rc + ")"
          end
        end
      end
      if mname == "sort"
        if recv_type == "sym_array"
          tmp = new_temp
          emit("  sp_IntArray *" + tmp + " = sp_IntArray_dup(" + rc + "); sp_sym_array_sort(" + tmp + ");")
          return tmp
        end
        return "sp_IntArray_sort(" + rc + ")"
      end
      if mname == "first"
        return "sp_IntArray_get(" + rc + ", 0)"
      end
      if mname == "last"
        return "sp_IntArray_get(" + rc + ", sp_IntArray_length(" + rc + ") - 1)"
      end
      if mname == "min"
        return "sp_IntArray_min(" + rc + ")"
      end
      if mname == "max"
        return "sp_IntArray_max(" + rc + ")"
      end
      if mname == "sum"
        return "sp_IntArray_sum(" + rc + ")"
      end
      if mname == "to_a"
        return "sp_IntArray_dup(" + rc + ")"
      end
      if mname == "uniq"
        return "sp_IntArray_uniq(" + rc + ")"
      end
      if mname == "join"
        jarg = compile_arg0(nid)
        if jarg == "0"
          jarg = "\"\""
        end
        return "sp_IntArray_join(" + rc + ", " + jarg + ")"
      end
      if mname == "reverse"
        return "({ sp_IntArray *_r = sp_IntArray_dup(" + rc + "); sp_IntArray_reverse_bang(_r); _r; })"
      end
      if mname == "compact"
        return "sp_IntArray_dup(" + rc + ")"
      end
      if mname == "flatten"
        return "sp_IntArray_dup(" + rc + ")"
      end
      if mname == "unshift"
        return "(sp_IntArray_unshift(" + rc + ", " + compile_arg0(nid) + "), 0)"
      end
      if mname == "dup"
        return "sp_IntArray_dup(" + rc + ")"
      end
      if mname == "count"
        return "sp_IntArray_length(" + rc + ")"
      end
      if mname == "min_by"
        if @nd_block[nid] >= 0
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          tmp = new_temp
          itmp = new_temp
          emit("  mrb_int " + tmp + " = sp_IntArray_get(" + rc + ", 0);")
          emit("  { mrb_int _best = INT64_MAX;")
          emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < sp_IntArray_length(" + rc + "); " + itmp + "++) {")
          emit("    mrb_int lv_" + bp + " = sp_IntArray_get(" + rc + ", " + itmp + ");")
          bbody = @nd_body[blk]
          bexpr = "0"
          if bbody >= 0
            bs = get_stmts(bbody)
            if bs.length > 0
              bexpr = compile_expr(bs.last)
            end
          end
          emit("    mrb_int _v = " + bexpr + ";")
          emit("    if (_v < _best) { _best = _v; " + tmp + " = lv_" + bp + "; }")
          emit("  } }")
          return tmp
        end
      end
      if mname == "max_by"
        if @nd_block[nid] >= 0
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          tmp = new_temp
          itmp = new_temp
          emit("  mrb_int " + tmp + " = sp_IntArray_get(" + rc + ", 0);")
          emit("  { mrb_int _best = INT64_MIN;")
          emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < sp_IntArray_length(" + rc + "); " + itmp + "++) {")
          emit("    mrb_int lv_" + bp + " = sp_IntArray_get(" + rc + ", " + itmp + ");")
          bbody = @nd_body[blk]
          bexpr = "0"
          if bbody >= 0
            bs = get_stmts(bbody)
            if bs.length > 0
              bexpr = compile_expr(bs.last)
            end
          end
          emit("    mrb_int _v = " + bexpr + ";")
          emit("    if (_v > _best) { _best = _v; " + tmp + " = lv_" + bp + "; }")
          emit("  } }")
          return tmp
        end
      end
      if mname == "sort_by"
        if @nd_block[nid] >= 0
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          tmp = new_temp
          emit("  sp_IntArray *" + tmp + " = sp_IntArray_dup(" + rc + ");")
          # Use bubble sort with block as key function
          emit("  { mrb_int _n = " + tmp + "->len;")
          emit("  for (mrb_int _i = 0; _i < _n - 1; _i++)")
          emit("    for (mrb_int _j = 0; _j < _n - 1 - _i; _j++) {")
          emit("      mrb_int lv_" + bp + " = " + tmp + "->data[" + tmp + "->start + _j];")
          bbody = @nd_body[blk]
          bexpr = "0"
          if bbody >= 0
            bs = get_stmts(bbody)
            if bs.length > 0
              bexpr = compile_expr(bs.last)
            end
          end
          emit("      mrb_int _ka = " + bexpr + ";")
          emit("      lv_" + bp + " = " + tmp + "->data[" + tmp + "->start + _j + 1];")
          emit("      mrb_int _kb = " + bexpr + ";")
          emit("      if (_ka > _kb) { mrb_int _t = " + tmp + "->data[" + tmp + "->start + _j]; " + tmp + "->data[" + tmp + "->start + _j] = " + tmp + "->data[" + tmp + "->start + _j + 1]; " + tmp + "->data[" + tmp + "->start + _j + 1] = _t; }")
          emit("    }")
          emit("  }")
          return tmp
        end
      end
    end
    # Float array methods
    if recv_type == "float_array"
      if mname == "length"
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_FloatArray_length(" + rc + ")"
      end
      if mname == "[]"
        # a[range] / a[start, len] return slices; bare a[i] stays a get.
        args_id = @nd_arguments[nid]
        if args_id >= 0
          a = get_args(args_id)
          if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
            left = compile_expr(@nd_left[a[0]])
            right = compile_expr(@nd_right[a[0]])
            adj = range_excl_end(a[0]) == 1 ? "" : " + 1"
            return "sp_FloatArray_slice(" + rc + ", " + left + ", " + right + " - " + left + adj + ")"
          end
          if a.length >= 2
            return "sp_FloatArray_slice(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
          end
        end
        return "sp_FloatArray_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "push"
        return "(sp_FloatArray_push(" + rc + ", " + compile_arg0(nid) + "), 0)"
      end
      if mname == "pop"
        return "sp_FloatArray_pop(" + rc + ")"
      end
      if mname == "empty?"
        return "sp_FloatArray_empty(" + rc + ")"
      end
      if mname == "size"
        return "sp_FloatArray_length(" + rc + ")"
      end
      if mname == "min"
        return "sp_FloatArray_min(" + rc + ")"
      end
      if mname == "max"
        return "sp_FloatArray_max(" + rc + ")"
      end
      if mname == "sum"
        return "sp_FloatArray_sum(" + rc + ")"
      end
      if mname == "first"
        return "sp_FloatArray_get(" + rc + ", 0)"
      end
      if mname == "last"
        return "sp_FloatArray_get(" + rc + ", -1)"
      end
    end
    if is_ptr_array_type(recv_type) == 1
      elem_type = ptr_array_elem_type(recv_type)
      ct = c_type(elem_type)
      if mname == "length" || mname == "size"
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_PtrArray_length(" + rc + ")"
      end
      if mname == "[]"
        return "((" + ct + ")sp_PtrArray_get(" + rc + ", " + compile_arg0(nid) + "))"
      end
      if mname == "push"
        return "(sp_PtrArray_push(" + rc + ", " + compile_arg0(nid) + "), 0)"
      end
      if mname == "empty?"
        return "sp_PtrArray_empty(" + rc + ")"
      end
    end
    if recv_type == "str_array"
      if mname == "length"
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_StrArray_length(" + rc + ")"
      end
      if mname == "size"
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_StrArray_length(" + rc + ")"
      end
      if mname == "[]"
        # a[range] / a[start, len] return slices; bare a[i] stays a get.
        args_id = @nd_arguments[nid]
        if args_id >= 0
          a = get_args(args_id)
          if a.length >= 1 && @nd_type[a[0]] == "RangeNode"
            left = compile_expr(@nd_left[a[0]])
            right = compile_expr(@nd_right[a[0]])
            adj = range_excl_end(a[0]) == 1 ? "" : " + 1"
            return "sp_StrArray_slice(" + rc + ", " + left + ", " + right + " - " + left + adj + ")"
          end
          if a.length >= 2
            return "sp_StrArray_slice(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
          end
        end
        return "sp_StrArray_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "first"
        return "sp_StrArray_get(" + rc + ", 0)"
      end
      if mname == "last"
        return "sp_StrArray_get(" + rc + ", sp_StrArray_length(" + rc + ") - 1)"
      end
      if mname == "join"
        jarg = compile_arg0(nid)
        if jarg == "0"
          jarg = "\"\""
        end
        return "sp_StrArray_join(" + rc + ", " + jarg + ")"
      end
      if mname == "push"
        return "(sp_StrArray_push(" + rc + ", " + compile_arg0(nid) + "), 0)"
      end
      if mname == "pop"
        return "sp_StrArray_pop(" + rc + ")"
      end
      if mname == "empty?"
        return "sp_StrArray_empty(" + rc + ")"
      end
      if mname == "include?"
        return "sp_StrArray_include(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "index" || mname == "find_index"
        if @nd_arguments[nid] >= 0
          return "sp_StrArray_index(" + rc + ", " + compile_arg0(nid) + ")"
        end
      end
      if mname == "rindex"
        if @nd_arguments[nid] >= 0
          return "sp_StrArray_rindex(" + rc + ", " + compile_arg0(nid) + ")"
        end
      end
      if mname == "delete_at"
        return "sp_StrArray_delete_at(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "tally"
        @needs_str_int_hash = 1
        return "sp_StrArray_tally(" + rc + ")"
      end
      if mname == "compact"
        return "sp_StrArray_compact(" + rc + ")"
      end
      if mname == "insert"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length >= 2
            return "(sp_StrArray_insert(" + rc + ", " + compile_expr(aargs[0]) + ", " + compile_expr(aargs[1]) + "), " + rc + ")"
          end
        end
      end
      if mname == "count"
        return "sp_StrArray_length(" + rc + ")"
      end
    end

    # PolyArray methods
    if recv_type == "poly_array"
      if mname == "length"
        return "sp_PolyArray_length(" + rc + ")"
      end
      if mname == "[]"
        return "sp_PolyArray_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
    end
    ""
  end

  def compile_hash_method_expr(nid, mname, rc, recv_type)
    # Hash methods
    if recv_type == "sym_int_hash"
      if mname == "[]"
        args_id0 = @nd_arguments[nid]
        if args_id0 >= 0
          aa0 = get_args(args_id0)
          if aa0.length > 0 && infer_type(aa0[0]) != "symbol"
            return "((mrb_int)0)"
          end
        end
        return "sp_SymIntHash_get((sp_SymIntHash *)(" + rc + "), " + compile_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        args_id1 = @nd_arguments[nid]
        if args_id1 >= 0
          aa1 = get_args(args_id1)
          if aa1.length > 0 && infer_type(aa1[0]) != "symbol"
            return "FALSE"
          end
        end
        return "sp_SymIntHash_has_key((sp_SymIntHash *)(" + rc + "), " + compile_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_SymIntHash_length((sp_SymIntHash *)(" + rc + "))"
      end
      if mname == "empty?"
        return "(sp_SymIntHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_SymIntHash_length(" + rc + ") > 0)"
      end
      if mname == "fetch"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          key = compile_expr(aargs[0])
          if aargs.length >= 2
            defval = compile_expr(aargs[1])
            return "(sp_SymIntHash_has_key(" + rc + ", " + key + ") ? sp_SymIntHash_get(" + rc + ", " + key + ") : " + defval + ")"
          end
          return "sp_SymIntHash_get((sp_SymIntHash *)(" + rc + "), " + key + ")"
        end
      end
    end
    if recv_type == "sym_str_hash"
      if mname == "[]"
        args_id0s = @nd_arguments[nid]
        if args_id0s >= 0
          aa0s = get_args(args_id0s)
          if aa0s.length > 0 && infer_type(aa0s[0]) != "symbol"
            return "(&(\"\\xff\")[1])"
          end
        end
        return "sp_SymStrHash_get((sp_SymStrHash *)(" + rc + "), " + compile_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        args_id1s = @nd_arguments[nid]
        if args_id1s >= 0
          aa1s = get_args(args_id1s)
          if aa1s.length > 0 && infer_type(aa1s[0]) != "symbol"
            return "FALSE"
          end
        end
        return "sp_SymStrHash_has_key((sp_SymStrHash *)(" + rc + "), " + compile_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_SymStrHash_length((sp_SymStrHash *)(" + rc + "))"
      end
      if mname == "empty?"
        return "(sp_SymStrHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_SymStrHash_length(" + rc + ") > 0)"
      end
      if mname == "fetch"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          key = compile_expr(aargs[0])
          if aargs.length >= 2
            defval = compile_expr(aargs[1])
            return "(sp_SymStrHash_has_key(" + rc + ", " + key + ") ? sp_SymStrHash_get(" + rc + ", " + key + ") : " + defval + ")"
          end
          return "sp_SymStrHash_get((sp_SymStrHash *)(" + rc + "), " + key + ")"
        end
      end
    end
    if recv_type == "sym_poly_hash"
      if mname == "[]"
        return "sp_SymPolyHash_get((sp_SymPolyHash *)(" + rc + "), " + compile_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        return "sp_SymPolyHash_has_key((sp_SymPolyHash *)(" + rc + "), " + compile_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        return "sp_SymPolyHash_length((sp_SymPolyHash *)(" + rc + "))"
      end
      if mname == "empty?"
        return "(sp_SymPolyHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_SymPolyHash_length(" + rc + ") > 0)"
      end
    end
    if recv_type == "str_poly_hash"
      if mname == "[]"
        return "sp_StrPolyHash_get(" + rc + ", " + compile_str_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        return "sp_StrPolyHash_has_key(" + rc + ", " + compile_str_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        return "sp_StrPolyHash_length(" + rc + ")"
      end
      if mname == "empty?"
        return "(sp_StrPolyHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_StrPolyHash_length(" + rc + ") > 0)"
      end
      if mname == "keys"
        return "sp_StrPolyHash_keys(" + rc + ")"
      end
    end
    if recv_type == "str_int_hash"
      if mname == "[]"
        return "sp_StrIntHash_get(" + rc + ", " + compile_str_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        return "sp_StrIntHash_has_key(" + rc + ", " + compile_str_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_StrIntHash_length(" + rc + ")"
      end
      if mname == "empty?"
        return "(sp_StrIntHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_StrIntHash_length(" + rc + ") > 0)"
      end
      if mname == "keys"
        return "sp_StrIntHash_keys(" + rc + ")"
      end
      if mname == "values"
        return "sp_StrIntHash_values(" + rc + ")"
      end
      if (mname == "select" || mname == "reject") && @nd_block[nid] >= 0
        return compile_hash_select_reject(nid, "str_int_hash", rc, mname)
      end
      if (mname == "count" || mname == "any?" || mname == "all?" || mname == "find" || mname == "detect") && @nd_block[nid] >= 0
        return compile_hash_block_predicate(nid, "str_int_hash", rc, mname)
      end
      if mname == "fetch"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          key = compile_expr_as_string(aargs[0])
          if aargs.length >= 2
            defval = compile_expr(aargs[1])
            return "(sp_StrIntHash_has_key(" + rc + ", " + key + ") ? sp_StrIntHash_get(" + rc + ", " + key + ") : " + defval + ")"
          end
          return "sp_StrIntHash_get(" + rc + ", " + key + ")"
        end
      end
      if mname == "merge"
        tmp = new_temp
        arg = compile_arg0(nid)
        emit("  sp_StrIntHash *" + tmp + " = sp_StrIntHash_merge(" + rc + ", " + arg + ");")
        return tmp
      end
      if mname == "to_a"
        tt = "tuple:string,int"
        register_tuple_type(tt)
        @needs_gc = 1
        tname = tuple_c_name(tt)
        tmp = new_temp
        itmp = new_temp
        emit("  sp_PtrArray *" + tmp + " = sp_PtrArray_new();")
        emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
        emit("    " + tname + " *_tp = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
        emit("    _tp->_0 = " + rc + "->order[" + itmp + "];")
        emit("    _tp->_1 = sp_StrIntHash_get(" + rc + ", " + rc + "->order[" + itmp + "]);")
        emit("    sp_PtrArray_push(" + tmp + ", _tp);")
        emit("  }")
        return tmp
      end
      if mname == "transform_values"
        if @nd_block[nid] >= 0
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          tmp = new_temp
          emit("  sp_StrIntHash *" + tmp + " = sp_StrIntHash_new();")
          emit("  for (mrb_int _i = 0; _i < " + rc + "->len; _i++) {")
          emit("    mrb_int lv_" + bp + " = sp_StrIntHash_get(" + rc + ", " + rc + "->order[_i]);")
          push_scope
          declare_var(bp, "int")
          bbody = @nd_body[blk]
          bexpr = "0"
          if bbody >= 0
            bs = get_stmts(bbody)
            if bs.length > 0
              bexpr = compile_expr(bs.last)
            end
          end
          emit("    sp_StrIntHash_set(" + tmp + ", " + rc + "->order[_i], " + bexpr + ");")
          pop_scope
          emit("  }")
          return tmp
        end
      end
    end
    if recv_type == "int_str_hash"
      @needs_int_str_hash = 1
      if mname == "[]"
        return "sp_IntStrHash_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        return "sp_IntStrHash_has_key(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        return "sp_IntStrHash_length(" + rc + ")"
      end
      if mname == "empty?"
        return "(sp_IntStrHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_IntStrHash_length(" + rc + ") > 0)"
      end
      if mname == "keys"
        @needs_int_array = 1
        return "sp_IntStrHash_keys(" + rc + ")"
      end
      if mname == "values"
        @needs_str_array = 1
        return "sp_IntStrHash_values(" + rc + ")"
      end
      if mname == "fetch"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          key = compile_expr(aargs[0])
          if aargs.length >= 2
            defval = compile_expr(aargs[1])
            return "(sp_IntStrHash_has_key(" + rc + ", " + key + ") ? sp_IntStrHash_get(" + rc + ", " + key + ") : " + defval + ")"
          end
          return "sp_IntStrHash_get(" + rc + ", " + key + ")"
        end
      end
    end
    if recv_type == "str_str_hash"
      if mname == "[]"
        return "sp_StrStrHash_get(" + rc + ", " + compile_str_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?" || mname == "include?" || mname == "member?"
        return "sp_StrStrHash_has_key(" + rc + ", " + compile_str_arg0(nid) + ")"
      end
      if mname == "length" || mname == "size" || (mname == "count" && @nd_block[nid] < 0 && @nd_arguments[nid] < 0)
        if @hoisted_strlen_var != "" && @hoisted_strlen_recv == rc
          return @hoisted_strlen_var
        end
        return "sp_StrStrHash_length(" + rc + ")"
      end
      if mname == "empty?"
        return "(sp_StrStrHash_length(" + rc + ") == 0)"
      end
      if mname == "any?" && @nd_block[nid] < 0
        return "(sp_StrStrHash_length(" + rc + ") > 0)"
      end
      if mname == "keys"
        return "sp_StrStrHash_keys(" + rc + ")"
      end
      if mname == "values"
        return "sp_StrStrHash_values(" + rc + ")"
      end
      if mname == "invert"
        return "sp_StrStrHash_invert(" + rc + ")"
      end
      if mname == "to_a"
        tt = "tuple:string,string"
        register_tuple_type(tt)
        @needs_gc = 1
        tname = tuple_c_name(tt)
        tmp = new_temp
        itmp = new_temp
        emit("  sp_PtrArray *" + tmp + " = sp_PtrArray_new();")
        emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
        emit("    " + tname + " *_tp = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
        emit("    _tp->_0 = " + rc + "->order[" + itmp + "];")
        emit("    _tp->_1 = sp_StrStrHash_get(" + rc + ", " + rc + "->order[" + itmp + "]);")
        emit("    sp_PtrArray_push(" + tmp + ", _tp);")
        emit("  }")
        return tmp
      end
      if (mname == "select" || mname == "reject") && @nd_block[nid] >= 0
        return compile_hash_select_reject(nid, "str_str_hash", rc, mname)
      end
      if (mname == "count" || mname == "any?" || mname == "all?" || mname == "find" || mname == "detect") && @nd_block[nid] >= 0
        return compile_hash_block_predicate(nid, "str_str_hash", rc, mname)
      end
      if mname == "fetch"
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          key = compile_expr_as_string(aargs[0])
          if aargs.length >= 2
            defval = compile_expr(aargs[1])
            return "(sp_StrStrHash_has_key(" + rc + ", " + key + ") ? sp_StrStrHash_get(" + rc + ", " + key + ") : " + defval + ")"
          end
          return "sp_StrStrHash_get(" + rc + ", " + key + ")"
        end
      end
    end
    ""
  end

  def compile_enumerable_expr(nid, mname)
    # map as expression
    if mname == "map"
      if @nd_block[nid] >= 0
        return compile_map_expr(nid)
      end
    end

    # flat_map as expression
    if mname == "flat_map"
      if @nd_block[nid] >= 0
        return compile_flat_map_expr(nid)
      end
    end

    # each_with_object as expression: run the loop as side effect, return obj
    if mname == "each_with_object"
      if @nd_block[nid] >= 0
        return compile_each_with_object_block(nid)
      end
    end

    # tap: run block with receiver, return receiver
    if mname == "tap"
      if @nd_block[nid] >= 0
        return compile_tap_expr(nid)
      end
    end

    # then / yield_self: pass receiver to block, return block result
    if mname == "then" || mname == "yield_self"
      if @nd_block[nid] >= 0
        return compile_then_expr(nid)
      end
    end

    # select as expression
    if mname == "select" || mname == "filter"
      if @nd_block[nid] >= 0
        return compile_select_expr(nid)
      end
    end

    # reject as expression
    if mname == "reject"
      if @nd_block[nid] >= 0
        return compile_reject_expr(nid)
      end
    end

    # reduce/inject as expression
    if mname == "reduce"
      if @nd_block[nid] >= 0
        return compile_reduce_expr(nid)
      end
    end
    if mname == "inject"
      if @nd_block[nid] >= 0
        return compile_reduce_expr(nid)
      end
    end
    ""
  end

  def compile_constant_recv_expr(nid, mname, recv, rc)
    rcname = constructor_class_name(recv)
    if rcname != ""
      # ARGV methods
      if rcname == "ARGV"
        if mname == "length"
          return "sp_argv.len"
        end
        if mname == "[]"
          idx_expr = compile_arg0(nid)
          return "((" + idx_expr + " < sp_argv.len) ? sp_argv.data[(int)" + idx_expr + "] : NULL)"
        end
      end
      # Math
      if rcname == "Math"
        if mname == "sqrt"
          return "sqrt(" + compile_arg0(nid) + ")"
        end
        if mname == "cos"
          return "cos(" + compile_arg0(nid) + ")"
        end
        if mname == "sin"
          return "sin(" + compile_arg0(nid) + ")"
        end
        if mname == "tan"
          return "tan(" + compile_arg0(nid) + ")"
        end
        if mname == "acos"
          return "acos(" + compile_arg0(nid) + ")"
        end
        if mname == "asin"
          return "asin(" + compile_arg0(nid) + ")"
        end
        if mname == "atan"
          return "atan(" + compile_arg0(nid) + ")"
        end
        if mname == "log"
          return "log(" + compile_arg0(nid) + ")"
        end
        if mname == "log2"
          return "log2(" + compile_arg0(nid) + ")"
        end
        if mname == "log10"
          return "log10(" + compile_arg0(nid) + ")"
        end
        if mname == "exp"
          return "exp(" + compile_arg0(nid) + ")"
        end
        if mname == "atan2"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            arg_ids = get_args(args_id)
            if arg_ids.length >= 2
              return "atan2(" + compile_expr(arg_ids[0]) + ", " + compile_expr(arg_ids[1]) + ")"
            end
          end
        end
        if mname == "hypot"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            arg_ids = get_args(args_id)
            if arg_ids.length >= 2
              return "hypot(" + compile_expr(arg_ids[0]) + ", " + compile_expr(arg_ids[1]) + ")"
            end
          end
        end
      end
      # File operations
      if rcname == "File"
        if mname == "read"
          return "sp_file_read(" + compile_arg0(nid) + ")"
        end
        if mname == "exist?"
          return "sp_file_exist(" + compile_arg0(nid) + ")"
        end
        if mname == "delete"
          return "(sp_file_delete(" + compile_arg0(nid) + "), 0)"
        end
        if mname == "join"
          args_id = @nd_arguments[nid]
          if args_id >= 0
            arg_ids = get_args(args_id)
            if arg_ids.length >= 2
              return "sp_str_concat(sp_str_concat(" + compile_expr(arg_ids[0]) + ", \"/\"), " + compile_expr(arg_ids[1]) + ")"
            end
          end
          return "\"\""
        end
        if mname == "basename"
          return "sp_file_basename(" + compile_arg0(nid) + ")"
        end
      end
      # Time
      if rcname == "Time"
        if mname == "now"
          return "((mrb_int)time(NULL))"
        end
      end
      # ENV
      if rcname == "ENV"
        if mname == "[]"
          return "sp_str_dup_external(getenv(" + compile_arg0(nid) + "))"
        end
      end
      # Dir
      if rcname == "Dir"
        if mname == "home"
          return "sp_str_dup_external(getenv(\"HOME\"))"
        end
      end
      # Module class method dispatch
      mi2 = 0
      while mi2 < @module_names.length
        if @module_names[mi2] == rcname
          # Look for module class method
          mfn = rcname + "_cls_" + mname
          mfi = find_method_idx(mfn)
          if mfi >= 0
            ca = compile_call_args(nid)
            if ca != ""
              return "sp_" + sanitize_name(mfn) + "(" + ca + ")"
            else
              return "sp_" + sanitize_name(mfn) + "()"
            end
          end
        end
        mi2 = mi2 + 1
      end
      # Class method dispatch (def self.xxx)
      ci3 = find_class_idx(rcname)
      if ci3 >= 0
        cmnames = @cls_cmeth_names[ci3].split(";")
        cj = 0
        while cj < cmnames.length
          if cmnames[cj] == mname
            ca = compile_call_args(nid)
            if ca != ""
              return "sp_" + rcname + "_cls_" + sanitize_name(mname) + "(" + ca + ")"
            else
              return "sp_" + rcname + "_cls_" + sanitize_name(mname) + "()"
            end
          end
          cj = cj + 1
        end
      end
    end
    ""
  end

  def compile_to_a_range_expr(nid, recv)
    range_nid = -1
    if @nd_type[recv] == "RangeNode"
      range_nid = recv
    end
    if @nd_type[recv] == "ParenthesesNode"
      pb = @nd_body[recv]
      if pb >= 0
        pstmts = get_stmts(pb)
        if pstmts.length > 0
          if @nd_type[pstmts.first] == "RangeNode"
            range_nid = pstmts.first
          end
        end
      end
    end
    if range_nid >= 0
      @needs_int_array = 1
      @needs_gc = 1
      right_expr = compile_expr(@nd_right[range_nid])
      # sp_IntArray_from_range is inclusive; for `1...3` shave the upper end.
      if range_excl_end(range_nid) == 1
        right_expr = "(" + right_expr + ") - 1"
      end
      return "sp_IntArray_from_range(" + compile_expr(@nd_left[range_nid]) + ", " + right_expr + ")"
    end
    ""
  end

  def compile_open_class_dispatch_expr(nid, mname, rc, recv_type)
    # Open class method dispatch on built-in types
    oc_prefix = ""
    if recv_type == "int"
      oc_prefix = "__oc_Integer_"
    end
    if recv_type == "string"
      oc_prefix = "__oc_String_"
    end
    if recv_type == "float"
      oc_prefix = "__oc_Float_"
    end
    if oc_prefix != ""
      oc_name = oc_prefix + mname
      oc_mi = find_method_idx(oc_name)
      if oc_mi >= 0
        ca = compile_call_args(nid)
        if ca != ""
          return "sp_" + sanitize_name(oc_name) + "(" + rc + ", " + ca + ")"
        else
          return "sp_" + sanitize_name(oc_name) + "(" + rc + ")"
        end
      end
    end
    ""
  end

  def compile_introspection_expr(nid, mname, rc, recv_type)
    # is_a? - check class hierarchy
    if mname == "is_a?"
      if is_obj_type(recv_type) == 1
        cname = recv_type[4, recv_type.length - 4]
        arg0 = ""
        args_id = @nd_arguments[nid]
        if args_id >= 0
          a = get_args(args_id)
          if a.length > 0
            arg0 = @nd_name[a[0]]
          end
        end
        # Check if cname is or inherits from arg0
        if is_class_or_ancestor(cname, arg0) == 1
          return "TRUE"
        else
          return "FALSE"
        end
      end
      return "FALSE"
    end

    # respond_to? - check if method exists
    if mname == "respond_to?"
      if is_obj_type(recv_type) == 1
        cname = recv_type[4, recv_type.length - 4]
        ci = find_class_idx(cname)
        if ci >= 0
          arg0 = ""
          args_id = @nd_arguments[nid]
          if args_id >= 0
            a = get_args(args_id)
            if a.length > 0
              arg0 = @nd_content[a[0]]
            end
          end
          if cls_find_method(ci, arg0) >= 0
            return "TRUE"
          end
          # Check attr_readers
          readers = @cls_attr_readers[ci].split(";")
          rk = 0
          while rk < readers.length
            if readers[rk] == arg0
              return "TRUE"
            end
            rk = rk + 1
          end
          return "FALSE"
        end
      end
      return "FALSE"
    end

    # nil?
    if mname == "nil?"
      if recv_type == "nil"
        return "TRUE"
      end
      if recv_type == "poly"
        return "sp_poly_nil_p(" + rc + ")"
      end
      if is_nullable_type(recv_type) == 1
        return "(" + rc + " == NULL)"
      end
      if type_is_pointer(recv_type) == 1
        return "(" + rc + " == NULL)"
      end
      return "FALSE"
    end

    # frozen? on any type
    if mname == "frozen?"
      return "TRUE"
    end

    # `freeze` on any object — returns the receiver. The string /
    # mutable-str dispatchers earlier in compile_call_expr handle their
    # own variants; this catches everything else (obj_*, sp_Object *,
    # array types, etc.) so `expr.freeze` in a const initializer (issue
    # #63) doesn't fall through to the "0" fallback.
    if mname == "freeze"
      return rc
    end

    # positive? / negative?
    if mname == "positive?"
      return "(" + rc + " > 0)"
    end
    if mname == "negative?"
      return "(" + rc + " < 0)"
    end
    ""
  end

  def compile_object_method_expr(nid, mname, rc, recv_type)
    # Object method calls
    if is_obj_type(recv_type) == 1
      bt = base_type(recv_type)
      cname = bt[4, bt.length - 4]
      ci = find_class_idx(cname)
      if ci >= 0
        arrow = "->"
        if @cls_is_value_type[ci] == 1
          arrow = "."
        end
        # attr_reader
        readers = @cls_attr_readers[ci].split(";")
        j = 0
        while j < readers.length
          if readers[j] == mname
            return rc + arrow + sanitize_ivar(mname)
          end
          j = j + 1
        end
        # attr_writer
        if mname.length > 1
          if mname[mname.length - 1] == "="
            bname = mname[0, mname.length - 1]
            writers = @cls_attr_writers[ci].split(";")
            j = 0
            while j < writers.length
              if writers[j] == bname
                return "(" + rc + arrow + sanitize_ivar(bname) + " = " + compile_arg0(nid) + ", 0)"
              end
              j = j + 1
            end
          end
        end
        # Method call
        owner = find_method_owner(ci, mname)
        if owner != ""
          oci2 = find_class_idx(owner)
          midx2 = -1
          if oci2 >= 0
            midx2 = cls_find_method_direct(oci2, mname)
          end
          # Omit the trailing &block slot from default-padding when the
          # callee declares one — we'll fill it explicitly from the
          # call site's literal block below.
          has_proc = cls_method_has_block_param(oci2, midx2)
          ca = ""
          if midx2 >= 0
            ca = compile_typed_call_args(nid, oci2, midx2, has_proc)
          else
            ca = compile_call_args(nid)
          end
          bp = ""
          if has_proc == 1
            bp = block_forward_expr(nid)
            if bp == ""
              # The callee declares &block but the call site provides
              # none — fill the slot with NULL so the C call has the
              # right arity.
              bp = "0"
            end
          end
          tail = build_call_tail(ca, bp)
          if owner == cname
            return "sp_" + owner + "_" + sanitize_name(mname) + "(" + rc + tail + ")"
          else
            return "sp_" + owner + "_" + sanitize_name(mname) + "((sp_" + owner + " *)" + rc + tail + ")"
          end
        end
      end
    end
    ""
  end

  def compile_int_class_fallback_expr(nid, mname, rc, recv_type)
    # Fallback: if receiver is int (e.g. from IntArray get) but method belongs to a class,
    # cast the int to the appropriate class pointer and dispatch
    if recv_type == "int"
      ci2 = 0
      while ci2 < @cls_names.length
        cname2 = @cls_names[ci2]
        readers2 = @cls_attr_readers[ci2].split(";")
        found_reader = 0
        j2 = 0
        while j2 < readers2.length
          if readers2[j2] == mname
            found_reader = 1
          end
          j2 = j2 + 1
        end
        if found_reader == 1
          return "((sp_" + cname2 + " *)" + rc + ")->" + sanitize_ivar(mname)
        end
        # Check writers
        if mname.length > 1
          if mname[mname.length - 1] == "="
            bname2 = mname[0, mname.length - 1]
            writers2 = @cls_attr_writers[ci2].split(";")
            j2 = 0
            while j2 < writers2.length
              if writers2[j2] == bname2
                return "(((sp_" + cname2 + " *)" + rc + ")->" + sanitize_ivar(bname2) + " = " + compile_arg0(nid) + ")"
              end
              j2 = j2 + 1
            end
          end
        end
        # Check methods
        owner2 = find_method_owner(ci2, mname)
        if owner2 != ""
          oci3 = find_class_idx(owner2)
          midx3 = -1
          if oci3 >= 0
            midx3 = cls_find_method_direct(oci3, mname)
          end
          ca2 = ""
          if midx3 >= 0
            ca2 = compile_typed_call_args(nid, oci3, midx3, 0)
          else
            ca2 = compile_call_args(nid)
          end
          if ca2 != ""
            return "sp_" + owner2 + "_" + sanitize_name(mname) + "((sp_" + owner2 + " *)" + rc + ", " + ca2 + ")"
          else
            return "sp_" + owner2 + "_" + sanitize_name(mname) + "((sp_" + owner2 + " *)" + rc + ")"
          end
        end
        ci2 = ci2 + 1
      end
    end
    ""
  end


  # Inferred return type for `recv.mname(...)` when `recv` is poly.
  # If every user class that defines mname agrees on the return type,
  # that concrete type is used. If any two disagree, the call is
  # genuinely polymorphic and the caller must treat the result as
  # an sp_RbVal.
  def poly_dispatch_return_type(mname)
    common = ""
    ci = 0
    while ci < @cls_names.length
      if cls_find_method_direct(ci, mname) >= 0
        rt = cls_method_return(ci, mname)
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

  def compile_poly_method_call(nid, rc, mname)
    @needs_rb_value = 1
    if mname == "nil?"
      return "sp_poly_nil_p(" + rc + ")"
    end
    if mname == "to_s"
      return "sp_poly_to_s(" + rc + ")"
    end
    # For object method calls, dispatch based on cls_id. Two namespaces
    # of cls_id share SP_TAG_OBJ:
    #   - non-negative: index into @cls_names (user-defined classes)
    #   - negative SP_BUILTIN_*: built-in pointer types (IntArray, ...)
    # The result temp is typed by the method's return type. If user
    # classes disagree on that type, the result is sp_RbVal and each
    # branch boxes its concrete return value.
    ret_type = poly_dispatch_return_type(mname)
    is_poly_ret = ret_type == "poly" ? 1 : 0
    ret_ct = c_type(ret_type)
    ret_def = c_default_val(ret_type)
    # Stash the receiver in a temp so we don't re-evaluate the
    # expression for every if-branch below.
    recv_tmp = new_temp
    emit("  sp_RbVal " + recv_tmp + " = " + rc + ";")
    # Compile the call's argument list once.
    arg_compiled = "".split(",")
    arg_strs = ""
    args_id = @nd_arguments[nid]
    if args_id >= 0
      aargs = get_args(args_id)
      k = 0
      while k < aargs.length
        ce = compile_expr(aargs[k])
        arg_compiled.push(ce)
        arg_strs = arg_strs + ", " + ce
        k = k + 1
      end
    end
    tmp = new_temp
    emit("  " + ret_ct + " " + tmp + " = " + ret_def + ";")
    emit("  if (" + recv_tmp + ".tag == SP_TAG_OBJ) {")
    # User-class dispatch
    i = 0
    while i < @cls_names.length
      cname = @cls_names[i]
      midx = cls_find_method_direct(i, mname)
      if midx >= 0
        call_expr = "sp_" + cname + "_" + sanitize_name(mname) + "((sp_" + cname + " *)" + recv_tmp + ".v.p" + arg_strs + ")"
        rhs = call_expr
        if is_poly_ret == 1
          this_rt = cls_method_return(i, mname)
          rhs = box_val_to_poly(call_expr, this_rt)
        end
        emit("    if (" + recv_tmp + ".cls_id == " + i.to_s + ") " + tmp + " = " + rhs + ";")
      end
      i = i + 1
    end
    # Built-in type dispatch (cls_id < 0).
    emit_poly_builtin_dispatch(recv_tmp, mname, arg_compiled, tmp, is_poly_ret)
    emit("  }")
    tmp
  end

  # Emit branches for the built-in (negative cls_id) entries. Each
  # entry maps a (SP_BUILTIN_*, method) pair to a C expression.
  # Adding a new built-in type means one more `if` branch here.
  def emit_poly_builtin_dispatch(recv_tmp, mname, arg_compiled, result_tmp, is_poly_ret)
    a0 = ""
    if arg_compiled.length > 0
      a0 = arg_compiled[0]
    end
    # IntArray: `[]`, `length`, `size`
    if mname == "[]" && arg_compiled.length >= 1
      call = "sp_IntArray_get((sp_IntArray *)" + recv_tmp + ".v.p, " + a0 + ")"
      rhs = is_poly_ret == 1 ? "sp_box_int(" + call + ")" : call
      emit("    if (" + recv_tmp + ".cls_id == SP_BUILTIN_INT_ARRAY) " + result_tmp + " = " + rhs + ";")
    end
    if mname == "length" || mname == "size"
      call = "sp_IntArray_length((sp_IntArray *)" + recv_tmp + ".v.p)"
      rhs = is_poly_ret == 1 ? "sp_box_int(" + call + ")" : call
      emit("    if (" + recv_tmp + ".cls_id == SP_BUILTIN_INT_ARRAY) " + result_tmp + " = " + rhs + ";")
    end
  end

  # Try to compile str[i] <op> "c" as direct char comparison
  # Returns "" if not applicable
  def try_char_cmp(nid, c_op)
    recv = @nd_receiver[nid]
    args_id = @nd_arguments[nid]
    if args_id < 0
      return ""
    end
    a = get_args(args_id)
    if a.length == 0
      return ""
    end
    arg_id = a[0]
    if @nd_type[arg_id] != "StringNode"
      return ""
    end
    lit = @nd_content[arg_id]
    if lit == ""
      lit = @nd_unescaped[arg_id]
    end
    if lit.length != 1
      return ""
    end
    if @nd_type[recv] != "CallNode" || @nd_name[recv] != "[]"
      return ""
    end
    sr = @nd_receiver[recv]
    if sr < 0 || infer_type(sr) != "string"
      return ""
    end
    str_c = compile_expr(sr)
    idx_c = compile_arg0(recv)
    ch = lit
    if lit == "\n"
      ch = "\\n"
    elsif lit == "\t"
      ch = "\\t"
    elsif lit == "\r"
      ch = "\\r"
    elsif lit.ord == 92 || lit.ord == 39
      # backslash or single quote — skip char optimization
      return ""
    end
    "(" + str_c + "[(mrb_int)" + idx_c + "] " + c_op + " '" + ch + "')"
  end

  def compile_eq(nid, op)
    recv = @nd_receiver[nid]
    lt = infer_type(recv)
    args_id = @nd_arguments[nid]
    arg_id = -1
    if args_id >= 0
      a = get_args(args_id)
      if a.length > 0
        arg_id = a[0]
      end
    end
    at = "int"
    if arg_id >= 0
      at = infer_type(arg_id)
    end
    lc = compile_expr(recv)
    rc = "0"
    if arg_id >= 0
      rc = compile_expr(arg_id)
    end
    # Symbol equality: distinct from all non-symbol types in Ruby.
    if lt == "symbol"
      if at == "symbol"
        if op == "=="
          return "(" + lc + " == " + rc + ")"
        else
          return "(" + lc + " != " + rc + ")"
        end
      end
      # sym vs non-sym: always unequal in Ruby
      return op == "==" ? "FALSE" : "TRUE"
    end
    if at == "symbol"
      # non-sym lhs vs sym rhs: also always unequal
      return op == "==" ? "FALSE" : "TRUE"
    end
    if lt == "string"
      if at == "nil"
        if op == "=="
          return "(" + lc + " == NULL)"
        else
          return "(" + lc + " != NULL)"
        end
      end
      # Optimize: str[i] == "c" → direct char comparison (no malloc)
      cc = try_char_cmp(nid, op)
      if cc != ""
        return cc
      end
      if op == "=="
        return "(strcmp(" + lc + ", " + rc + ") == 0)"
      else
        return "(strcmp(" + lc + ", " + rc + ") != 0)"
      end
    end
    if at == "nil"
      if type_is_pointer(lt) == 1
        if op == "=="
          return "(" + lc + " == NULL)"
        else
          return "(" + lc + " != NULL)"
        end
      end
    end
    if lt == "int_array"
      if at == "int_array"
        if op == "=="
          return "sp_IntArray_eq(" + lc + ", " + rc + ")"
        else
          return "(!sp_IntArray_eq(" + lc + ", " + rc + "))"
        end
      end
    end
    "(" + lc + " " + op + " " + rc + ")"
  end

  # Box an already-compiled value of static type `at` into an sp_RbVal.
  # Mirrors box_expr_to_poly but operates on a raw (type, value) pair so
  # callers that already have temps don't have to re-emit the expr.
  def box_value_to_poly(at, val)
    if at == "poly"
      return val
    end
    if at == "int"
      return "sp_box_int(" + val + ")"
    end
    if at == "string"
      return "sp_box_str(" + val + ")"
    end
    if at == "float"
      return "sp_box_float(" + val + ")"
    end
    if at == "bool"
      return "sp_box_bool(" + val + ")"
    end
    if at == "nil"
      return "sp_box_nil()"
    end
    if at == "symbol"
      return "sp_box_sym(" + val + ")"
    end
    # Built-in pointer types route through sp_box_obj with a reserved
    # negative cls_id (mirrors box_expr_to_poly).
    if at == "int_array"
      return "sp_box_int_array(" + val + ")"
    end
    if at == "float_array"
      return "sp_box_float_array(" + val + ")"
    end
    if at == "str_array"
      return "sp_box_str_array(" + val + ")"
    end
    if at == "sym_array"
      return "sp_box_sym_array(" + val + ")"
    end
    if is_ptr_array_type(at) == 1
      return "sp_box_ptr_array(" + val + ")"
    end
    if at == "proc" || at == "lambda"
      return "sp_box_proc(" + val + ")"
    end
    if is_obj_type(at) == 1
      cname = at[4, at.length - 4]
      ci = find_class_idx(cname)
      return "sp_box_obj(" + val + ", " + ci.to_s + ")"
    end
    "sp_box_int(" + val + ")"
  end

  def box_expr_to_poly(nid)
    at = infer_type(nid)
    val = compile_expr(nid)
    if at == "poly"
      return val
    end
    if at == "int"
      return "sp_box_int(" + val + ")"
    end
    if at == "string"
      return "sp_box_str(" + val + ")"
    end
    if at == "float"
      return "sp_box_float(" + val + ")"
    end
    if at == "bool"
      return "sp_box_bool(" + val + ")"
    end
    if at == "nil"
      return "sp_box_nil()"
    end
    if at == "symbol"
      return "sp_box_sym(" + val + ")"
    end
    if is_obj_type(at) == 1
      cname = at[4, at.length - 4]
      ci = find_class_idx(cname)
      return "sp_box_obj(" + val + ", " + ci.to_s + ")"
    end
    # Built-in pointer types: route through sp_box_obj with a reserved
    # negative cls_id (SP_BUILTIN_*) so dispatch is uniform.
    if at == "int_array"
      return "sp_box_int_array(" + val + ")"
    end
    if at == "float_array"
      return "sp_box_float_array(" + val + ")"
    end
    if at == "str_array"
      return "sp_box_str_array(" + val + ")"
    end
    if at == "sym_array"
      return "sp_box_sym_array(" + val + ")"
    end
    if is_ptr_array_type(at) == 1
      return "sp_box_ptr_array(" + val + ")"
    end
    if at == "proc" || at == "lambda"
      return "sp_box_proc(" + val + ")"
    end
    "sp_box_int(" + val + ")"
  end

  def box_val_to_poly(val, at)
    if at == "poly"
      return val
    end
    if at == "int"
      return "sp_box_int(" + val + ")"
    end
    if at == "string"
      return "sp_box_str(" + val + ")"
    end
    if at == "float"
      return "sp_box_float(" + val + ")"
    end
    if at == "bool"
      return "sp_box_bool(" + val + ")"
    end
    if at == "nil"
      return "sp_box_nil()"
    end
    if at == "symbol"
      return "sp_box_sym(" + val + ")"
    end
    "sp_box_int(" + val + ")"
  end

  # Emit a runtime loop that pushes every element of the array `src_expr`
  # (a node id whose value is some typed array) onto the destination
  # int_array variable `dst`. Used when expanding `*args` into a rest
  # parameter that will be received as sp_IntArray *.
  def emit_splat_into_int_array(dst, src_expr)
    src_t = infer_type(src_expr)
    src_v = compile_expr(src_expr)
    @needs_int_array = 1
    @needs_gc = 1
    if src_t == "int_array" || src_t == "sym_array"
      i = new_temp
      emit("  for (mrb_int " + i + " = 0; " + i + " < sp_IntArray_length(" + src_v + "); " + i + "++) sp_IntArray_push(" + dst + ", sp_IntArray_get(" + src_v + ", " + i + "));")
      return
    end
    if src_t == "str_array"
      @needs_str_array = 1
      i = new_temp
      emit("  for (mrb_int " + i + " = 0; " + i + " < sp_StrArray_length(" + src_v + "); " + i + "++) sp_IntArray_push(" + dst + ", (mrb_int)sp_StrArray_get(" + src_v + ", " + i + "));")
      return
    end
    if src_t == "float_array"
      @needs_float_array = 1
      i = new_temp
      emit("  for (mrb_int " + i + " = 0; " + i + " < sp_FloatArray_length(" + src_v + "); " + i + "++) sp_IntArray_push(" + dst + ", (mrb_int)sp_FloatArray_get(" + src_v + ", " + i + "));")
      return
    end
    if is_ptr_array_type(src_t) == 1
      i = new_temp
      emit("  for (mrb_int " + i + " = 0; " + i + " < sp_PtrArray_length(" + src_v + "); " + i + "++) sp_IntArray_push(" + dst + ", (mrb_int)(intptr_t)sp_PtrArray_get(" + src_v + ", " + i + "));")
      return
    end
    if src_t == "poly_array"
      i = new_temp
      emit("  for (mrb_int " + i + " = 0; " + i + " < sp_PolyArray_length(" + src_v + "); " + i + "++) sp_IntArray_push(" + dst + ", sp_PolyArray_get(" + src_v + ", " + i + ").v.i);")
      return
    end
    # Fallback: treat the single value as one element.
    emit("  sp_IntArray_push(" + dst + ", (mrb_int)" + src_v + ");")
  end

  # Read an element of a typed array as an mrb_int (so it fits int param
  # slots and the int_array rest bundle uniformly).
  def array_get_as_int_expr(src_t, src_v, idx_expr)
    if src_t == "int_array" || src_t == "sym_array"
      return "sp_IntArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if src_t == "str_array"
      return "(mrb_int)sp_StrArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if src_t == "float_array"
      return "(mrb_int)sp_FloatArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if is_ptr_array_type(src_t) == 1
      return "(mrb_int)(intptr_t)sp_PtrArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if src_t == "poly_array"
      # Pull the int channel out of the tagged union. Lossy for non-int
      # tags — Spinel's *rest can only hold mrb_int today, so any non-int
      # element splatted into a rest param prints as raw bits.
      return "sp_PolyArray_get(" + src_v + ", " + idx_expr + ").v.i"
    end
    "0"
  end

  # Same as array_get_as_int_expr but returns the element in its native
  # C type (used when the param slot is typed, e.g. const char *).
  def array_get_native_expr(src_t, src_v, idx_expr)
    if src_t == "int_array" || src_t == "sym_array"
      return "sp_IntArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if src_t == "str_array"
      return "sp_StrArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if src_t == "float_array"
      return "sp_FloatArray_get(" + src_v + ", " + idx_expr + ")"
    end
    if is_ptr_array_type(src_t) == 1
      return "sp_PtrArray_get(" + src_v + ", " + idx_expr + ")"
    end
    "0"
  end

  # Splat-aware companion to compile_call_args_with_defaults. Handles a
  # single SplatNode in positional args. The conceptual positional list
  # is (prefix... ++ splat_array ++ suffix...); fixed params eat from the
  # left; the rest param (if any) gets the remainder.
  def compile_call_args_splat(nid, mi, pnames, ptypes, defaults, kw_names, kw_vals, positional_ids, splat_idx)
    splat_node = positional_ids[splat_idx]
    splat_src_id = @nd_expression[splat_node]
    prefix_count = splat_idx
    suffix_count = positional_ids.length - splat_idx - 1

    # Pre-evaluate the splat source so we can index it twice (length and
    # element access) without re-evaluating side effects.
    src_t = "int_array"
    src_v = "0"
    if splat_src_id >= 0
      src_t = infer_type(splat_src_id)
      @needs_gc = 1
      src_tmp = new_temp
      emit("  " + c_type(src_t) + " " + src_tmp + " = " + compile_expr(splat_src_id) + ";")
      if type_is_pointer(src_t) == 1
        emit("  SP_GC_ROOT(" + src_tmp + ");")
      end
      src_v = src_tmp
    end
    src_len_expr = length_c_expr(src_t, src_v)
    if src_len_expr == ""
      src_len_expr = "0"
    end

    # Identify if the last param is a rest int_array.
    method_has_rest = 0
    if pnames.length > 0
      if ptypes[pnames.length - 1] == "int_array"
        method_has_rest = 1
      end
    end
    n_fixed = pnames.length
    if method_has_rest == 1
      n_fixed = pnames.length - 1
    end

    result = ""
    k = 0
    while k < pnames.length
      if k > 0
        result = result + ", "
      end

      # Keyword args take priority (matches non-splat path).
      kw_found = 0
      ki = 0
      while ki < kw_names.length
        if kw_names[ki] == pnames[k]
          kw_found = 1
          if k < ptypes.length && ptypes[k] == "poly"
            result = result + "sp_box_str(" + kw_vals[ki] + ")"
          else
            result = result + kw_vals[ki]
          end
        end
        ki = ki + 1
      end
      if kw_found == 1
        k = k + 1
        next
      end

      # Rest param: bundle leftover splat elements + suffix positionals.
      if k == pnames.length - 1 && method_has_rest == 1
        @needs_int_array = 1
        @needs_gc = 1
        rest_tmp = new_temp
        emit("  sp_IntArray *" + rest_tmp + " = sp_IntArray_new();")
        # Prefix positionals beyond n_fixed overflow into the rest before
        # any splat content (e.g. take(1, 2, *xs) where take has only a
        # *rest param: the literals 1 and 2 must lead the bundle).
        po_start = n_fixed
        if po_start < 0
          po_start = 0
        end
        if po_start < prefix_count
          poi = po_start
          while poi < prefix_count
            emit("  sp_IntArray_push(" + rest_tmp + ", (mrb_int)" + compile_expr(positional_ids[poi]) + ");")
            poi = poi + 1
          end
        end
        consumed = n_fixed - prefix_count
        if consumed < 0
          consumed = 0
        end
        i_loop = new_temp
        emit("  for (mrb_int " + i_loop + " = " + consumed.to_s + "; " + i_loop + " < " + src_len_expr + "; " + i_loop + "++) sp_IntArray_push(" + rest_tmp + ", " + array_get_as_int_expr(src_t, src_v, i_loop) + ");")
        si = 0
        while si < suffix_count
          pid = positional_ids[splat_idx + 1 + si]
          emit("  sp_IntArray_push(" + rest_tmp + ", (mrb_int)" + compile_expr(pid) + ");")
          si = si + 1
        end
        result = result + rest_tmp
        k = k + 1
        next
      end

      # Fixed param. Determine which conceptual positional it consumes.
      pt = "int"
      if k < ptypes.length
        pt = ptypes[k]
      end
      if k < prefix_count
        if pt == "poly"
          result = result + box_expr_to_poly(positional_ids[k])
        else
          result = result + compile_expr(positional_ids[k])
        end
        k = k + 1
        next
      end
      # Index into the splat source.
      idx_in_splat = k - prefix_count
      # Unconsumed splat elements available for fixed params:
      #   src_len - (positional slots after the splat that need to come
      #              from the splat to feed remaining fixed params)
      # We don't know src_len statically, so we trust the caller has
      # provided enough — a runtime over-read returns 0/NULL via the
      # array's bounds clamp.
      slots_left_for_splat = n_fixed - prefix_count
      if idx_in_splat < slots_left_for_splat
        ge = array_get_native_expr(src_t, src_v, idx_in_splat.to_s)
        if pt == "poly"
          # The splat element itself isn't a node id, so wrap manually.
          result = result + ge
        else
          result = result + ge
        end
        k = k + 1
        next
      end
      # Comes from a suffix positional (after the splat).
      suffix_offset = idx_in_splat - slots_left_for_splat
      pid_idx = splat_idx + 1 + suffix_offset
      if pid_idx < positional_ids.length
        if pt == "poly"
          result = result + box_expr_to_poly(positional_ids[pid_idx])
        else
          result = result + compile_expr(positional_ids[pid_idx])
        end
      else
        # Fall back to default if defined.
        if k < defaults.length
          def_id = defaults[k].to_i
          if def_id >= 0
            result = result + compile_expr(def_id)
          else
            result = result + "0"
          end
        else
          result = result + "0"
        end
      end
      k = k + 1
    end
    result
  end

  def compile_call_args_with_defaults(nid, mi, omit_trailing = 0)
    # `omit_trailing` is the number of trailing param slots to leave out
    # entirely — block-forwarding call sites pass 1 so the &block slot
    # isn't default-padded with "0" (the actual proc is appended by the
    # caller after this returns).
    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end
    pnames = @meth_param_names[mi].split(",")
    ptypes = @meth_param_types[mi].split(",")
    defaults = @meth_has_defaults[mi].split(",")
    if omit_trailing > 0
      kept = "".split(",")
      pk = 0
      limit = pnames.length - omit_trailing
      if limit < 0
        limit = 0
      end
      while pk < limit
        kept.push(pnames[pk])
        pk = pk + 1
      end
      pnames = kept
    end

    # Check if args contain a KeywordHashNode - extract kw pairs
    kw_names = "".split(",")
    kw_vals = "".split(",")
    positional_ids = []
    splat_idx = -1
    splat_count_local = 0
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
              kw_names.push(kname)
              kw_vals.push(compile_expr(@nd_expression[elems[ek]]))
            end
          end
          ek = ek + 1
        end
      else
        if @nd_type[arg_ids[ak]] == "SplatNode"
          if splat_idx < 0
            splat_idx = positional_ids.length
          end
          splat_count_local = splat_count_local + 1
        end
        positional_ids.push(arg_ids[ak])
      end
      ak = ak + 1
    end

    if splat_count_local == 1
      return compile_call_args_splat(nid, mi, pnames, ptypes, defaults, kw_names, kw_vals, positional_ids, splat_idx)
    end

    result = ""
    k = 0
    while k < pnames.length
      if k > 0
        result = result + ", "
      end
      # Check keyword args first
      kw_found = 0
      ki = 0
      while ki < kw_names.length
        if kw_names[ki] == pnames[k]
          kw_found = 1
          # Check if param is poly
          if k < ptypes.length
            if ptypes[k] == "poly"
              # Need to box - kw_vals[ki] is already compiled
              result = result + "sp_box_str(" + kw_vals[ki] + ")"
            else
              result = result + kw_vals[ki]
            end
          else
            result = result + kw_vals[ki]
          end
        end
        ki = ki + 1
      end
      if kw_found == 0
        if k < ptypes.length
          if ptypes[k] == "int_array"
            # Rest parameter (splat). Trigger when caller passes more
            # positional args than the method has params, OR when any
            # positional arg is itself a SplatNode that we have to expand.
            has_splat_arg = 0
            si = k
            while si < positional_ids.length
              if @nd_type[positional_ids[si]] == "SplatNode"
                has_splat_arg = 1
              end
              si = si + 1
            end
            # Treat the last param as a rest target when it's the trailing
            # int_array slot. This covers three cases:
            #   - extra positional args spilling in (the original heuristic)
            #   - a SplatNode somewhere in the args
            #   - no args supplied at all (rest-only method called bare)
            is_last_param = 0
            if k == pnames.length - 1
              is_last_param = 1
            end
            treat_as_rest = 0
            if positional_ids.length > pnames.length || has_splat_arg == 1
              treat_as_rest = 1
            end
            if is_last_param == 1 && positional_ids.length <= k
              treat_as_rest = 1
            end
            if treat_as_rest == 1
              # Fast path: the only positional is a splat whose source is
              # already an int_array. Pass it directly without copying.
              if has_splat_arg == 1 && positional_ids.length == k + 1 && @nd_type[positional_ids[k]] == "SplatNode"
                src_expr = @nd_expression[positional_ids[k]]
                if src_expr >= 0
                  src_t = infer_type(src_expr)
                  if src_t == "int_array" || src_t == "sym_array"
                    result = result + compile_expr(src_expr)
                    k = k + 1
                    next
                  end
                end
              end
              @needs_int_array = 1
              @needs_gc = 1
              tmp = new_temp
              emit("  sp_IntArray *" + tmp + " = sp_IntArray_new();")
              pi = k
              while pi < positional_ids.length
                if @nd_type[positional_ids[pi]] == "SplatNode"
                  src_expr2 = @nd_expression[positional_ids[pi]]
                  if src_expr2 >= 0
                    emit_splat_into_int_array(tmp, src_expr2)
                  end
                else
                  emit("  sp_IntArray_push(" + tmp + ", (mrb_int)" + compile_expr(positional_ids[pi]) + ");")
                end
                pi = pi + 1
              end
              result = result + tmp
              k = k + 1
              next
            end
          end
        end
        if k < positional_ids.length
          if k < ptypes.length
            if ptypes[k] == "poly"
              result = result + box_expr_to_poly(positional_ids[k])
              k = k + 1
              next
            end
            # Issue #58: empty `[]` literal at the call site needs to
            # construct the right typed-array container. The literal's
            # own infer_type returns int_array (compile_array_literal
            # emits sp_IntArray_new()), but if the param is a concrete
            # typed-array, emit the matching constructor instead.
            if is_empty_array_literal(positional_ids[k]) == 1
              if ptypes[k] == "str_array"
                @needs_str_array = 1
                @needs_gc = 1
                result = result + "sp_StrArray_new()"
                k = k + 1
                next
              end
              if ptypes[k] == "float_array"
                @needs_float_array = 1
                @needs_gc = 1
                result = result + "sp_FloatArray_new()"
                k = k + 1
                next
              end
              if ptypes[k] == "sym_array"
                @needs_int_array = 1
                @needs_gc = 1
                result = result + "sp_IntArray_new()"
                k = k + 1
                next
              end
            end
          end
          result = result + compile_expr(positional_ids[k])
        else
          # Use default value
          if k < defaults.length
            def_id = defaults[k].to_i
            if def_id >= 0
              result = result + compile_expr(def_id)
            else
              result = result + "0"
            end
          else
            result = result + "0"
          end
        end
      end
      k = k + 1
    end
    result
  end

  def compile_constructor_args(ci, nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      # No call-site args. If init has parameters with defaults, fill them
      # in here (issue #49: `Counter.new` for `initialize(start = 0)`).
      init_ci = find_init_class(ci)
      if init_ci >= 0
        init_idx = cls_find_method_direct(init_ci, "initialize")
        if init_idx >= 0
          return compile_typed_call_args(nid, init_ci, init_idx, 0)
        end
      end
      return ""
    end
    arg_ids = get_args(args_id)
    # Check if any arg is a KeywordHashNode
    has_kw = 0
    ak = 0
    while ak < arg_ids.length
      if @nd_type[arg_ids[ak]] == "KeywordHashNode"
        has_kw = 1
      end
      ak = ak + 1
    end
    if has_kw == 0
      # Positional args: still need to box any arg whose corresponding
      # ctor param is poly.
      init_ci_p = find_init_class(ci)
      if init_ci_p >= 0
        init_idx_p = cls_find_method_direct(init_ci_p, "initialize")
        if init_idx_p >= 0
          all_ptypes_p = @cls_meth_ptypes[init_ci_p].split("|")
          if init_idx_p < all_ptypes_p.length
            ptypes_p = all_ptypes_p[init_idx_p].split(",")
            has_poly = 0
            kpp = 0
            while kpp < ptypes_p.length
              if ptypes_p[kpp] == "poly"
                has_poly = 1
              end
              kpp = kpp + 1
            end
            if has_poly == 1
              result_p = ""
              kp = 0
              while kp < arg_ids.length
                if kp > 0
                  result_p = result_p + ", "
                end
                pt_p = "int"
                if kp < ptypes_p.length
                  pt_p = ptypes_p[kp]
                end
                if pt_p == "poly"
                  result_p = result_p + box_expr_to_poly(arg_ids[kp])
                else
                  result_p = result_p + compile_expr(arg_ids[kp])
                end
                kp = kp + 1
              end
              return result_p
            end
          end
        end
      end
      return compile_call_args(nid)
    end
    # Extract keyword pairs — remember the expression nid too so we can
    # box it (sp_box_int etc.) when the matching ctor param is poly.
    kw_names = "".split(",")
    kw_exprs = []
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
              kw_names.push(kname)
              kw_exprs.push(@nd_expression[elems[ek]])
            end
          end
          ek = ek + 1
        end
      end
      ak = ak + 1
    end
    # Get init param names/types from class
    init_ci = find_init_class(ci)
    if init_ci < 0
      return compile_call_args(nid)
    end
    init_idx = cls_find_method_direct(init_ci, "initialize")
    if init_idx < 0
      return compile_call_args(nid)
    end
    all_params = @cls_meth_params[init_ci].split("|")
    all_ptypes = @cls_meth_ptypes[init_ci].split("|")
    pnames = "".split(",")
    ptypes = "".split(",")
    if init_idx < all_params.length
      pnames = all_params[init_idx].split(",")
    end
    if init_idx < all_ptypes.length
      ptypes = all_ptypes[init_idx].split(",")
    end
    # Build args in param order using keyword values
    result = ""
    pk = 0
    while pk < pnames.length
      if pk > 0
        result = result + ", "
      end
      pt = "int"
      if pk < ptypes.length
        pt = ptypes[pk]
      end
      found = 0
      ki = 0
      while ki < kw_names.length
        if kw_names[ki] == pnames[pk]
          expr_id = kw_exprs[ki]
          if pt == "poly"
            result = result + box_expr_to_poly(expr_id)
          else
            result = result + compile_expr(expr_id)
          end
          found = 1
        end
        ki = ki + 1
      end
      if found == 0
        if pt == "poly"
          result = result + "sp_box_nil()"
        else
          result = result + "0"
        end
      end
      pk = pk + 1
    end
    result
  end

  def compile_call_args(nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      return ""
    end
    arg_ids = get_args(args_id)
    # Check if multiple args may trigger GC
    gc_count = 0
    k = 0
    while k < arg_ids.length
      if expr_may_gc(arg_ids[k]) == 1
        gc_count = gc_count + 1
      end
      k = k + 1
    end
    if gc_count >= 2
      return compile_gc_safe_args(arg_ids)
    end
    result = ""
    k = 0
    while k < arg_ids.length
      if k > 0
        result = result + ", "
      end
      result = result + compile_expr(arg_ids[k])
      k = k + 1
    end
    result
  end

  def compile_gc_safe_args(arg_ids)
    temps = "".split(",")
    k = 0
    while k < arg_ids.length
      if expr_may_gc(arg_ids[k]) == 1
        tmp = new_temp
        at = infer_type(arg_ids[k])
        ct = c_type(at)
        emit("  " + ct + " " + tmp + " = " + compile_expr(arg_ids[k]) + ";")
        if type_is_pointer(at) == 1
          emit("  SP_GC_ROOT(" + tmp + ");")
        end
        temps.push(tmp)
      else
        temps.push(compile_expr(arg_ids[k]))
      end
      k = k + 1
    end
    result = ""
    k = 0
    while k < temps.length
      if k > 0
        result = result + ", "
      end
      result = result + temps[k]
      k = k + 1
    end
    result
  end

  def compile_typed_call_args(nid, target_ci, target_midx, omit_trailing)
    # Like compile_call_args but casts arguments to match target method param
    # types AND fills in defaults from @cls_meth_defaults for trailing
    # parameters the caller omitted (issue #49). Returns "" only when the
    # method takes no parameters at all.
    #
    # `omit_trailing` is the number of trailing param slots to leave out
    # entirely — block-forwarding call sites pass 1 so the &block slot
    # isn't default-padded with "0" (the actual proc is appended by the
    # caller after this returns).
    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end
    all_ptypes = @cls_meth_ptypes[target_ci].split("|")
    all_defaults = @cls_meth_defaults[target_ci].split("|")
    ptypes = "".split(",")
    defaults = "".split(",")
    if target_midx < all_ptypes.length
      ptypes = all_ptypes[target_midx].split(",")
    end
    if target_midx < all_defaults.length
      defaults = all_defaults[target_midx].split(",")
    end
    # Drop the trailing slots the caller will fill explicitly (the
    # &block slot when block-forwarding) — otherwise a surplus
    # positional arg would mis-cast against the omitted slot's type
    # and the loop's default-padding would emit "0" for the slot the
    # caller is about to fill itself.
    if omit_trailing > 0
      kept = []
      pk = 0
      limit = ptypes.length - omit_trailing
      if limit < 0
        limit = 0
      end
      while pk < limit
        kept.push(ptypes[pk])
        pk = pk + 1
      end
      ptypes = kept
    end
    total = ptypes.length
    if arg_ids.length > total
      total = arg_ids.length
    end
    if total == 0
      return ""
    end
    result = ""
    pcname = ""
    k = 0
    while k < total
      if k > 0
        result = result + ", "
      end
      if k < arg_ids.length
        aexpr = compile_expr(arg_ids[k])
        at = infer_type(arg_ids[k])
        if k < ptypes.length
          pt = ptypes[k]
          if at == "int"
            if is_obj_type(pt) == 1
              # Cast int to object pointer
              pcname = pt[4, pt.length - 4]
              aexpr = "(sp_" + pcname + " *)" + aexpr
            end
          end
          if is_obj_type(at) == 1
            if pt == "int"
              # Cast object pointer to int
              aexpr = "(mrb_int)" + aexpr
            end
          end
        end
        result = result + aexpr
      else
        # Caller omitted this trailing arg — emit the method's default.
        if k < defaults.length
          def_id = defaults[k].to_i
          if def_id >= 0
            result = result + compile_expr(def_id)
          else
            result = result + "0"
          end
        else
          result = result + "0"
        end
      end
      k = k + 1
    end
    result
  end

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

  def compile_if_expr(nid)
    cond = compile_cond_expr(@nd_predicate[nid])
    then_val = "0"
    body = @nd_body[nid]
    if body >= 0
      stmts = get_stmts(body)
      if stmts.length > 0
        then_val = compile_expr(stmts.last)
      end
    end
    else_val = "0"
    sub = @nd_subsequent[nid]
    if sub >= 0
      if @nd_type[sub] == "ElseNode"
        eb = @nd_body[sub]
        if eb >= 0
          es = get_stmts(eb)
          if es.length > 0
            else_val = compile_expr(es.last)
          end
        end
      else
        else_val = compile_if_expr(sub)
      end
    end
    "(" + cond + " ? " + then_val + " : " + else_val + ")"
  end

  def compile_unless_expr(nid)
    cond = compile_cond_expr(@nd_predicate[nid])
    then_val = "0"
    body = @nd_body[nid]
    if body >= 0
      stmts = get_stmts(body)
      if stmts.length > 0
        then_val = compile_expr(stmts.last)
      end
    end
    "(!" + cond + " ? " + then_val + " : 0)"
  end

  def compile_array_literal(nid)
    @needs_gc = 1
    elems = parse_id_list(@nd_elements[nid])
    if elems.length == 0
      @needs_int_array = 1
      return "sp_IntArray_new()"
    end
    arr_type = infer_array_elem_type(nid)
    if is_tuple_type(arr_type) == 1
      name = tuple_c_name(arr_type)
      tmp = new_temp
      parts = tuple_elem_types_str(arr_type).split(",")
      emit("  " + name + " *" + tmp + " = (" + name + " *)sp_gc_alloc(sizeof(" + name + "), NULL, " + tuple_scan_name(arr_type) + ");")
      k = 0
      while k < elems.length && k < parts.length
        emit("  " + tmp + "->_" + k.to_s + " = " + compile_expr(elems[k]) + ";")
        k = k + 1
      end
      return tmp
    end
    if arr_type == "str_array"
      @needs_str_array = 1
      tmp = new_temp
      emit("  sp_StrArray *" + tmp + " = sp_StrArray_new();")
      k = 0
      while k < elems.length
        emit("  sp_StrArray_push(" + tmp + ", " + compile_expr(elems[k]) + ");")
        k = k + 1
      end
      return tmp
    end
    if arr_type == "poly_array"
      @needs_rb_value = 1
      tmp = new_temp
      emit("  sp_PolyArray *" + tmp + " = sp_PolyArray_new();")
      k = 0
      while k < elems.length
        et = infer_type(elems[k])
        val = compile_expr(elems[k])
        if et == "string"
          emit("  sp_PolyArray_push(" + tmp + ", sp_box_str(" + val + "));")
        elsif et == "float"
          emit("  sp_PolyArray_push(" + tmp + ", sp_box_float(" + val + "));")
        elsif et == "bool"
          emit("  sp_PolyArray_push(" + tmp + ", sp_box_bool(" + val + "));")
        elsif et == "nil"
          emit("  sp_PolyArray_push(" + tmp + ", sp_box_nil());")
        elsif et == "symbol"
          emit("  sp_PolyArray_push(" + tmp + ", sp_box_sym(" + val + "));")
        else
          emit("  sp_PolyArray_push(" + tmp + ", sp_box_int(" + val + "));")
        end
        k = k + 1
      end
      return tmp
    end
    if arr_type == "float_array"
      @needs_float_array = 1
      tmp = new_temp
      emit("  sp_FloatArray *" + tmp + " = sp_FloatArray_new();")
      k = 0
      while k < elems.length
        emit("  sp_FloatArray_push(" + tmp + ", " + compile_expr(elems[k]) + ");")
        k = k + 1
      end
      return tmp
    end
    if is_ptr_array_type(arr_type) == 1
      tmp = new_temp
      emit("  sp_PtrArray *" + tmp + " = sp_PtrArray_new();")
      k = 0
      while k < elems.length
        emit("  sp_PtrArray_push(" + tmp + ", " + compile_expr(elems[k]) + ");")
        k = k + 1
      end
      return tmp
    end
    @needs_int_array = 1
    tmp = new_temp
    emit("  sp_IntArray *" + tmp + " = sp_IntArray_new();")
    k = 0
    while k < elems.length
      emit("  sp_IntArray_push(" + tmp + ", " + compile_expr(elems[k]) + ");")
      k = k + 1
    end
    tmp
  end

  def compile_hash_literal(nid)
    @needs_gc = 1
    elems = parse_id_list(@nd_elements[nid])
    if elems.length == 0
      @needs_str_int_hash = 1
      return "sp_StrIntHash_new()"
    end
    ht = infer_hash_val_type(nid)
    if ht == "int_str_hash"
      @needs_int_str_hash = 1
      tmp = new_temp
      emit("  sp_IntStrHash *" + tmp + " = sp_IntStrHash_new();")
      elems.each { |el|
        if @nd_type[el] == "AssocNode"
          emit("  sp_IntStrHash_set(" + tmp + ", " + compile_expr(@nd_key[el]) + ", " + compile_expr(@nd_expression[el]) + ");")
        end
      }
      return tmp
    end
    if ht == "str_str_hash"
      @needs_str_str_hash = 1
      tmp = new_temp
      emit("  sp_StrStrHash *" + tmp + " = sp_StrStrHash_new();")
      elems.each { |el|
        if @nd_type[el] == "AssocNode"
          vt = infer_type(@nd_expression[el])
          val = compile_expr(@nd_expression[el])
          if vt == "int"
            val = "sp_int_to_s(" + val + ")"
          else
            if vt == "float"
              val = "sp_float_to_s(" + val + ")"
            else
              if vt == "bool"
                val = "(" + val + " ? \"true\" : \"false\")"
              end
            end
          end
          emit("  sp_StrStrHash_set(" + tmp + ", " + compile_expr_as_string(@nd_key[el]) + ", " + val + ");")
        end
      }
      return tmp
    end
    if ht == "sym_int_hash"
      @needs_sym_int_hash = 1
      tmp = new_temp
      emit("  sp_SymIntHash *" + tmp + " = sp_SymIntHash_new();")
      elems.each { |el|
        if @nd_type[el] == "AssocNode"
          emit("  sp_SymIntHash_set(" + tmp + ", " + compile_expr(@nd_key[el]) + ", " + compile_expr(@nd_expression[el]) + ");")
        end
      }
      return tmp
    end
    if ht == "sym_str_hash"
      @needs_sym_str_hash = 1
      tmp = new_temp
      emit("  sp_SymStrHash *" + tmp + " = sp_SymStrHash_new();")
      elems.each { |el|
        if @nd_type[el] == "AssocNode"
          emit("  sp_SymStrHash_set(" + tmp + ", " + compile_expr(@nd_key[el]) + ", " + compile_expr(@nd_expression[el]) + ");")
        end
      }
      return tmp
    end
    if ht == "sym_poly_hash"
      @needs_rb_value = 1
      tmp = new_temp
      emit("  sp_SymPolyHash *" + tmp + " = sp_SymPolyHash_new();")
      elems.each { |el|
        if @nd_type[el] == "AssocNode"
          emit("  sp_SymPolyHash_set(" + tmp + ", " + compile_expr(@nd_key[el]) + ", " + box_expr_to_poly(@nd_expression[el]) + ");")
        end
      }
      return tmp
    end
    if ht == "str_poly_hash"
      @needs_rb_value = 1
      tmp = new_temp
      emit("  sp_StrPolyHash *" + tmp + " = sp_StrPolyHash_new();")
      elems.each { |el|
        if @nd_type[el] == "AssocNode"
          emit("  sp_StrPolyHash_set(" + tmp + ", " + compile_expr_as_string(@nd_key[el]) + ", " + box_expr_to_poly(@nd_expression[el]) + ");")
        end
      }
      return tmp
    end
    @needs_str_int_hash = 1
    tmp = new_temp
    emit("  sp_StrIntHash *" + tmp + " = sp_StrIntHash_new();")
    elems.each { |el|
      if @nd_type[el] == "AssocNode"
        emit("  sp_StrIntHash_set(" + tmp + ", " + compile_expr_as_string(@nd_key[el]) + ", " + compile_expr(@nd_expression[el]) + ");")
      end
    }
    tmp
  end

  # ---- Statement compiler ----
  def compile_stmt(nid)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "MultiWriteNode"
      compile_multi_write(nid)
      return
    end
    if t == "GlobalVariableWriteNode"
      gname = @nd_name[nid]
      if gname != "$stderr" && gname != "$stdout" && gname != "$?"
        cname = sanitize_gvar(gname)
        val = compile_expr(@nd_expression[nid])
        emit("  " + cname + " = " + val + ";")
        return
      end
    end
    if t == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      # Check for method(:name) assignment
      expr_id = @nd_expression[nid]
      if expr_id >= 0
        if @nd_type[expr_id] == "CallNode"
          if @nd_name[expr_id] == "method"
            args_id = @nd_arguments[expr_id]
            if args_id >= 0
              arg_ids = get_args(args_id)
              if arg_ids.length >= 1
                mref = @nd_content[arg_ids[0]]
                if mref == ""
                  mref = @nd_name[arg_ids[0]]
                end
                @method_ref_vars.push(lname)
                @method_ref_names.push(mref)
                emit("  /* " + lname + " = method(:" + mref + ") */")
                return
              end
            end
          end
        end
      end
      vref = fiber_var_ref(lname)
      vt = find_var_type(lname)
      # Empty array literal: create the correct array type. Returning
      # early here also preserves the scope's already-promoted type
      # (issue #58, #85) — the fall-through path below would clobber
      # vt with infer_type([])'s "int_array" via set_var_type.
      if vt == "str_array" || vt == "float_array" || vt == "sym_array" || is_ptr_array_type(vt) == 1
        expr_id = @nd_expression[nid]
        if expr_id >= 0 && @nd_type[expr_id] == "ArrayNode"
          elems = parse_id_list(@nd_elements[expr_id])
          if elems.length == 0
            if vt == "str_array"
              @needs_str_array = 1
              @needs_gc = 1
              emit("  " + vref + " = sp_StrArray_new();")
            elsif vt == "float_array"
              @needs_float_array = 1
              @needs_gc = 1
              emit("  " + vref + " = sp_FloatArray_new();")
            elsif vt == "sym_array"
              # sym_array shares sp_IntArray storage.
              @needs_int_array = 1
              @needs_gc = 1
              emit("  " + vref + " = sp_IntArray_new();")
            else
              @needs_gc = 1
              emit("  " + vref + " = sp_PtrArray_new();")
            end
            return
          end
        end
      end
      # Empty hash literal: create the correct hash type
      if vt == "str_str_hash"
        expr_id2 = @nd_expression[nid]
        if expr_id2 >= 0 && @nd_type[expr_id2] == "HashNode"
          elems2 = parse_id_list(@nd_elements[expr_id2])
          if elems2.length == 0
            @needs_str_str_hash = 1
            @needs_gc = 1
            emit("  " + vref + " = sp_StrStrHash_new();")
            return
          end
        end
      end
      if vt == "bigint"
        rhs_t = infer_type(@nd_expression[nid])
        val = compile_expr(@nd_expression[nid])
        if rhs_t == "bigint"
          emit("  " + vref + " = " + val + ";")
        else
          emit("  " + vref + " = sp_bigint_new_int(" + val + ");")
        end
        # Trigger GC after bigint statement (safe point - all results stored in vars)
        emit("  if(sp_gc_bytes>sp_gc_threshold){size_t _b=sp_gc_bytes;sp_gc_collect();size_t _f=_b-sp_gc_bytes;if(_f<_b/4)sp_gc_threshold=_b*2;else if(sp_gc_bytes>0){sp_gc_threshold=sp_gc_bytes*4;if(sp_gc_threshold<sp_gc_threshold_init)sp_gc_threshold=sp_gc_threshold_init;}else sp_gc_threshold=sp_gc_threshold_init;}")
        return
      end
      if vt == "poly"
        # Box the value
        emit("  " + vref + " = " + box_expr_to_poly(@nd_expression[nid]) + ";")
        return
      end
      if vt == "mutable_str"
        rhs_type = infer_type(@nd_expression[nid])
        val = compile_expr(@nd_expression[nid])
        if rhs_type == "string" || rhs_type == "int"
          emit("  " + vref + " = sp_String_new(" + val + ");")
        else
          emit("  " + vref + " = " + val + ";")
        end
        return
      end
      # Optimize: x = str.split(sep) inside a loop → reuse StrArray
      if @in_loop == 1 && vt == "str_array"
        expr_id = @nd_expression[nid]
        if expr_id >= 0 && @nd_type[expr_id] == "CallNode" && @nd_name[expr_id] == "split"
          r = @nd_receiver[expr_id]
          if r >= 0 && infer_type(r) == "string"
            src = compile_expr(r)
            sep = compile_arg0(expr_id)
            emit("  if (" + vref + " == NULL) " + vref + " = sp_str_split(" + src + ", " + sep + ");")
            emit("  else sp_str_split_into(" + vref + ", " + src + ", " + sep + ");")
            return
          end
        end
      end
      rhs_t = infer_type(@nd_expression[nid])
      if rhs_t == "nil" && is_nullable_type(vt) == 1
        emit("  " + vref + " = NULL;")
      else
        val = compile_expr(@nd_expression[nid])
        emit("  " + vref + " = " + val + ";")
      end
      if rhs_t != "nil" || is_nullable_type(vt) == 0
        set_var_type(lname, rhs_t)
      end
      return
    end
    if t == "LocalVariableOperatorWriteNode"
      op = @nd_binop[nid]
      val = compile_expr(@nd_expression[nid])
      vref = fiber_var_ref(@nd_name[nid])
      vt = find_var_type(@nd_name[nid])
      if vt == "bigint"
        at = infer_type(@nd_expression[nid])
        barg = at == "bigint" ? val : "sp_bigint_new_int(" + val + ")"
        if op == "+"
          emit("  " + vref + " = sp_bigint_add(" + vref + ", " + barg + ");")
        end
        if op == "-"
          emit("  " + vref + " = sp_bigint_sub(" + vref + ", " + barg + ");")
        end
        if op == "*"
          emit("  " + vref + " = sp_bigint_mul(" + vref + ", " + barg + ");")
        end
        if op == "/"
          emit("  " + vref + " = sp_bigint_div(" + vref + ", " + barg + ");")
        end
        emit("  if(sp_gc_bytes>sp_gc_threshold){size_t _b=sp_gc_bytes;sp_gc_collect();size_t _f=_b-sp_gc_bytes;if(_f<_b/4)sp_gc_threshold=_b*2;else if(sp_gc_bytes>0){sp_gc_threshold=sp_gc_bytes*4;if(sp_gc_threshold<sp_gc_threshold_init)sp_gc_threshold=sp_gc_threshold_init;}else sp_gc_threshold=sp_gc_threshold_init;}")
        return
      end
      if op == "+"
        if vt == "string" && infer_type(@nd_expression[nid]) == "string"
          emit("  " + vref + " = sp_str_concat(" + vref + ", " + val + ");")
        else
          emit("  " + vref + " += " + val + ";")
        end
      end
      if op == "-"
        emit("  " + vref + " -= " + val + ";")
      end
      if op == "*"
        emit("  " + vref + " *= " + val + ";")
      end
      if op == "/"
        emit("  " + vref + " /= " + val + ";")
      end
      if op == "%"
        emit("  " + vref + " = sp_imod(" + vref + ", " + val + ");")
      end
      if op == "<<"
        emit("  " + vref + " <<= " + val + ";")
      end
      if op == ">>"
        emit("  " + vref + " >>= " + val + ";")
      end
      if op == "&"
        emit("  " + vref + " &= " + val + ";")
      end
      if op == "|"
        emit("  " + vref + " |= " + val + ";")
      end
      if op == "^"
        emit("  " + vref + " ^= " + val + ";")
      end
      return
    end
    if t == "InstanceVariableWriteNode"
      iname = @nd_name[nid]
      expr_id = @nd_expression[nid]
      # Empty `{}` literal assigned to an ivar that scan_writer_calls
      # has promoted to a non-default hash type. compile_hash_literal
      # always returns `sp_StrIntHash_new()` for empty `{}`, so without
      # this special-case the ivar slot's type and the initializer's
      # type disagree (issue #64).
      ivt = ""
      if @current_class_idx >= 0
        ivt = cls_ivar_type(@current_class_idx, iname)
      end
      if is_empty_hash_literal(expr_id) == 1 && ivt != "" && ivt != "str_int_hash"
        ctor = ""
        if ivt == "str_str_hash"
          @needs_str_str_hash = 1
          ctor = "sp_StrStrHash_new()"
        elsif ivt == "int_str_hash"
          @needs_int_str_hash = 1
          ctor = "sp_IntStrHash_new()"
        elsif ivt == "sym_int_hash"
          @needs_sym_int_hash = 1
          ctor = "sp_SymIntHash_new()"
        elsif ivt == "sym_str_hash"
          @needs_sym_str_hash = 1
          ctor = "sp_SymStrHash_new()"
        elsif ivt == "str_poly_hash"
          @needs_str_poly_hash = 1
          ctor = "sp_StrPolyHash_new()"
        elsif ivt == "sym_poly_hash"
          @needs_sym_poly_hash = 1
          ctor = "sp_SymPolyHash_new()"
        end
        if ctor != ""
          @needs_gc = 1
          emit("  " + self_arrow + sanitize_ivar(iname) + " = " + ctor + ";")
          return
        end
      end
      val = compile_expr(expr_id)
      # Check if we're in a module class method
      mod_ivar = 0
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          if @current_method_name.start_with?(mmod + "_cls_")
            cname3 = mmod + "_" + iname[1, iname.length - 1]
            ci3 = find_const_idx(cname3)
            if ci3 >= 0
              emit("  cst_" + cname3 + " = " + val + ";")
              mod_ivar = 1
            end
          end
        end
        mi3 = mi3 + 1
      end
      if mod_ivar == 0
        emit("  " + self_arrow + sanitize_ivar(iname) + " = " + val + ";")
      end
      return
    end
    if t == "InstanceVariableOperatorWriteNode"
      op = @nd_binop[nid]
      val = compile_expr(@nd_expression[nid])
      ivar = sanitize_ivar(@nd_name[nid])
      if op == "+"
        emit("  " + self_arrow + ivar + " += " + val + ";")
      end
      if op == "-"
        emit("  " + self_arrow + ivar + " -= " + val + ";")
      end
      return
    end
    # `recv[idx] OP= value`. Without this case, the IndexOperatorWriteNode
    # the parser emits would fall through and the increment would be
    # silently dropped — symptoms include matmul / += accumulators
    # producing zero-valued output even though forward passes look fine.
    if t == "IndexOperatorWriteNode"
      compile_index_op_assign(nid)
      return
    end
    if t == "IfNode"
      compile_if_stmt(nid)
      return
    end
    if t == "UnlessNode"
      compile_unless_stmt(nid)
      return
    end
    if t == "WhileNode"
      compile_while_stmt(nid)
      return
    end
    if t == "UntilNode"
      compile_until_stmt(nid)
      return
    end
    if t == "ForNode"
      compile_for_stmt(nid)
      return
    end
    if t == "CaseNode"
      compile_case_stmt(nid)
      return
    end
    if t == "CaseMatchNode"
      compile_case_match_stmt(nid)
      return
    end
    if t == "ReturnNode"
      compile_return_stmt(nid)
      return
    end
    if t == "BreakNode"
      emit("  break;")
      return
    end
    if t == "NextNode"
      emit("  continue;")
      return
    end
    if t == "RetryNode"
      emit("  continue;")
      return
    end
    if t == "YieldNode"
      compile_yield_stmt(nid)
      return
    end
    if t == "BeginNode"
      compile_begin_stmt(nid)
      return
    end
    if t == "CallNode"
      compile_call_stmt(nid)
      return
    end
    if t == "StatementsNode"
      stmts = parse_id_list(@nd_stmts[nid])
      k = 0
      while k < stmts.length
        compile_stmt(stmts[k])
        k = k + 1
      end
      return
    end
    if t == "ParenthesesNode"
      body = @nd_body[nid]
      if body >= 0
        pstmts = get_stmts(body)
        pk = 0
        while pk < pstmts.length
          compile_stmt(pstmts[pk])
          pk = pk + 1
        end
      end
      return
    end
    expr = compile_expr(nid)
    if expr != "0"
      emit("  " + expr + ";")
    end
    return
  end

  # Emit assignment of `value_expr` to a single MultiWrite target node
  # (LocalVariableTargetNode or InstanceVariableTargetNode). Centralized
  # so the splat path doesn't have to duplicate the InstanceVariable
  # special-cases (module-method-promoted ivar handling).
  # Assign `value_expr` (whose static C type is `value_type`) into the
  # multi-write target node. When the local target's slot is `poly` and
  # the source value isn't already boxed, the value is boxed first so a
  # heterogeneous RHS like `a, b, c = [1, "b", 2.0]` lands in the right
  # tagged-union slots.
  def emit_multi_write_target(tid, value_expr, value_type)
    if @nd_type[tid] == "LocalVariableTargetNode"
      lname = @nd_name[tid]
      vt = find_var_type(lname)
      v = value_expr
      if vt == "poly" && value_type != "" && value_type != "poly"
        v = box_value_to_poly(value_type, value_expr)
      end
      emit("  " + fiber_var_ref(lname) + " = " + v + ";")
      return
    end
    if @nd_type[tid] == "InstanceVariableTargetNode"
      iname = @nd_name[tid]
      mod_ivar = 0
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          if @current_method_name.start_with?(mmod + "_cls_")
            cname3 = mmod + "_" + iname[1, iname.length - 1]
            ci3 = find_const_idx(cname3)
            if ci3 >= 0
              emit("  cst_" + cname3 + " = " + value_expr + ";")
              mod_ivar = 1
            end
          end
        end
        mi3 = mi3 + 1
      end
      if mod_ivar == 0
        v = value_expr
        if @current_class_idx >= 0
          it = cls_ivar_type(@current_class_idx, iname)
          if it == "poly" && value_type != "" && value_type != "poly"
            v = box_value_to_poly(value_type, value_expr)
          end
        end
        emit("  " + self_arrow + sanitize_ivar(iname) + " = " + v + ";")
      end
    end
  end

  # Handle `a, *b = rhs` / `*a, b = rhs` / `a, *b, c = rhs`.
  # `lefts` are pre-splat targets, `rest_id` is the SplatNode (its
  # expression is the splat target), `rights` are post-splat targets.
  def compile_multi_write_splat(lefts, rest_id, rights, val_id)
    splat_target = @nd_expression[rest_id]
    nleft = lefts.length
    nright = rights.length

    # ArrayNode literal RHS — split statically.
    if @nd_type[val_id] == "ArrayNode"
      elems = parse_id_list(@nd_elements[val_id])
      n = elems.length
      # Evaluate all RHS into temps first (swap-safe).
      tmps = "".split(",")
      ttypes = "".split(",")
      k = 0
      while k < n
        tmp = new_temp
        tmps.push(tmp)
        et = infer_type(elems[k])
        ttypes.push(et)
        emit("  " + c_type(et) + " " + tmp + " = " + compile_expr(elems[k]) + ";")
        k = k + 1
      end
      # Pre-splat targets get the first `nleft` temps.
      k = 0
      while k < nleft
        if k < n
          emit_multi_write_target(lefts[k], tmps[k], ttypes[k])
        end
        k = k + 1
      end
      # Splat target receives a fresh array of the matching element type.
      mid_count = n - nleft - nright
      if mid_count < 0
        mid_count = 0
      end
      st_type = splat_rest_type(val_id)
      st_tmp = new_temp
      @needs_gc = 1
      if st_type == "str_array"
        @needs_str_array = 1
        emit("  sp_StrArray *" + st_tmp + " = sp_StrArray_new();")
        k = 0
        while k < mid_count
          emit("  sp_StrArray_push(" + st_tmp + ", " + tmps[nleft + k] + ");")
          k = k + 1
        end
      elsif st_type == "float_array"
        @needs_float_array = 1
        emit("  sp_FloatArray *" + st_tmp + " = sp_FloatArray_new();")
        k = 0
        while k < mid_count
          emit("  sp_FloatArray_push(" + st_tmp + ", " + tmps[nleft + k] + ");")
          k = k + 1
        end
      elsif is_ptr_array_type(st_type) == 1
        emit("  sp_PtrArray *" + st_tmp + " = sp_PtrArray_new();")
        k = 0
        while k < mid_count
          emit("  sp_PtrArray_push(" + st_tmp + ", " + tmps[nleft + k] + ");")
          k = k + 1
        end
      elsif st_type == "poly_array"
        emit("  sp_PolyArray *" + st_tmp + " = sp_PolyArray_new();")
        k = 0
        while k < mid_count
          boxed = box_value_to_poly(ttypes[nleft + k], tmps[nleft + k])
          emit("  sp_PolyArray_push(" + st_tmp + ", " + boxed + ");")
          k = k + 1
        end
      else
        # int_array / sym_array share IntArray storage via mrb_int
        # reinterpretation at compile time.
        @needs_int_array = 1
        emit("  sp_IntArray *" + st_tmp + " = sp_IntArray_new();")
        k = 0
        while k < mid_count
          emit("  sp_IntArray_push(" + st_tmp + ", (mrb_int)" + tmps[nleft + k] + ");")
          k = k + 1
        end
      end
      emit_multi_write_target(splat_target, st_tmp, st_type)
      # Post-splat targets get the trailing temps.
      k = 0
      while k < nright
        idx = n - nright + k
        if idx >= 0 && idx < n
          emit_multi_write_target(rights[k], tmps[idx], ttypes[idx])
        end
        k = k + 1
      end
      return
    end

    # Generic typed-array RHS — slice at runtime.
    rt = infer_type(val_id)
    @needs_gc = 1
    tmp = new_temp
    emit("  " + c_type(rt) + " " + tmp + " = " + compile_expr(val_id) + ";")
    emit("  SP_GC_ROOT(" + tmp + ");")
    len_tmp = new_temp
    emit("  mrb_int " + len_tmp + " = " + length_c_expr(rt, tmp) + ";")
    # Pre-splat targets. int_array / sym_array share IntArray storage,
    # so they share the default. Other typed arrays need their matching
    # `_get` / `_slice` so the C calls are well-typed.
    get_fn = "sp_IntArray_get"
    slice_fn = "sp_IntArray_slice"
    if rt == "str_array"
      get_fn = "sp_StrArray_get"
      slice_fn = "sp_StrArray_slice"
    end
    if rt == "float_array"
      get_fn = "sp_FloatArray_get"
      slice_fn = "sp_FloatArray_slice"
    end
    if rt == "poly_array"
      get_fn = "sp_PolyArray_get"
      slice_fn = "sp_PolyArray_slice"
    end
    if is_ptr_array_type(rt) == 1
      get_fn = "sp_PtrArray_get"
      slice_fn = "sp_PtrArray_slice"
    end
    elem_t = elem_type_of_array(rt)
    k = 0
    while k < nleft
      emit_multi_write_target(lefts[k], get_fn + "(" + tmp + ", " + k.to_s + ")", elem_t)
      k = k + 1
    end
    # Splat target gets a runtime slice.
    mid_len = len_tmp + " - " + (nleft + nright).to_s
    emit_multi_write_target(splat_target, slice_fn + "(" + tmp + ", " + nleft.to_s + ", " + mid_len + ")", rt)
    # Post-splat targets.
    k = 0
    while k < nright
      offset_expr = len_tmp + " - " + (nright - k).to_s
      emit_multi_write_target(rights[k], get_fn + "(" + tmp + ", " + offset_expr + ")", elem_t)
      k = k + 1
    end
  end

  def compile_multi_write(nid)
    targets = parse_id_list(@nd_targets[nid])
    val_id = @nd_expression[nid]
    if val_id < 0
      return
    end
    rest_id = @nd_rest[nid]
    if is_splat_with_target(rest_id) == 1
      rights = parse_id_list(@nd_rights[nid])
      compile_multi_write_splat(targets, rest_id, rights, val_id)
      return
    end
    if @nd_type[val_id] == "ArrayNode"
      # Direct array literal: a, b, c = [1, 2, 3] or a, b = b, a
      elems = parse_id_list(@nd_elements[val_id])
      # For swap safety, evaluate all RHS first into temps
      tmps = "".split(",")
      ttypes_lit = "".split(",")
      k = 0
      while k < elems.length
        tmp = new_temp
        tmps.push(tmp)
        et = infer_type(elems[k])
        ttypes_lit.push(et)
        emit("  " + c_type(et) + " " + tmp + " = " + compile_expr(elems[k]) + ";")
        k = k + 1
      end
      # Now assign — emit_multi_write_target boxes when target slot is poly.
      k = 0
      while k < targets.length
        if k < tmps.length
          emit_multi_write_target(targets[k], tmps[k], ttypes_lit[k])
        end
        k = k + 1
      end
    elsif is_tuple_type(infer_type(val_id)) == 1
      # RHS is a tuple-returning call — destructure via field access.
      val_t = infer_type(val_id)
      @needs_gc = 1
      tmp = new_temp
      emit("  " + c_type(val_t) + " " + tmp + " = " + compile_expr(val_id) + ";")
      emit("  SP_GC_ROOT(" + tmp + ");")
      k = 0
      while k < targets.length
        tid = targets[k]
        if @nd_type[tid] == "LocalVariableTargetNode"
          emit("  " + fiber_var_ref(@nd_name[tid]) + " = " + tmp + "->_" + k.to_s + ";")
        end
        k = k + 1
      end
    else
      # RHS is a function call returning int_array
      @needs_int_array = 1
      @needs_gc = 1
      tmp = new_temp
      emit("  sp_IntArray *" + tmp + " = " + compile_expr(val_id) + ";")
      emit("  SP_GC_ROOT(" + tmp + ");")
      k = 0
      while k < targets.length
        tid = targets[k]
        if @nd_type[tid] == "LocalVariableTargetNode"
          emit("  " + fiber_var_ref(@nd_name[tid]) + " = sp_IntArray_get(" + tmp + ", " + k.to_s + ");")
        end
        k = k + 1
      end
    end
  end

  def compile_if_stmt(nid)
    cond = compile_cond_expr(@nd_predicate[nid])
    emit("  if (" + cond + ") {")
    @indent = @indent + 1
    compile_stmts_body(@nd_body[nid])
    @indent = @indent - 1
    sub = @nd_subsequent[nid]
    if sub >= 0
      if @nd_type[sub] == "ElseNode"
        emit("  } else {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[sub])
        @indent = @indent - 1
      else
        emit("  } else")
        compile_if_stmt(sub)
        return
      end
    end
    emit("  }")
  end

  def compile_unless_stmt(nid)
    cond = compile_cond_expr(@nd_predicate[nid])
    emit("  if (!(" + cond + ")) {")
    @indent = @indent + 1
    compile_stmts_body(@nd_body[nid])
    @indent = @indent - 1
    ec = @nd_else_clause[nid]
    if ec >= 0
      emit("  } else {")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[ec])
      @indent = @indent - 1
    end
    emit("  }")
  end

  # C expression for computing length of a value of the given type.
  # Returns "" if the type doesn't have a hoist-friendly length op.
  def length_c_expr(rt, rc)
    if rt == "string"
      return "sp_str_length(" + rc + ")"
    end
    if rt == "int_array" || rt == "sym_array"
      return "sp_IntArray_length(" + rc + ")"
    end
    if rt == "str_array"
      return "sp_StrArray_length(" + rc + ")"
    end
    if rt == "float_array"
      return "sp_FloatArray_length(" + rc + ")"
    end
    if is_ptr_array_type(rt) == 1
      return "sp_PtrArray_length(" + rc + ")"
    end
    if rt == "poly_array"
      return "sp_PolyArray_length(" + rc + ")"
    end
    if rt == "str_int_hash"
      return "sp_StrIntHash_length(" + rc + ")"
    end
    if rt == "str_str_hash"
      return "sp_StrStrHash_length(" + rc + ")"
    end
    if rt == "int_str_hash"
      return "sp_IntStrHash_length(" + rc + ")"
    end
    if rt == "sym_int_hash"
      return "sp_SymIntHash_length((sp_SymIntHash *)(" + rc + "))"
    end
    if rt == "sym_str_hash"
      return "sp_SymStrHash_length((sp_SymStrHash *)(" + rc + "))"
    end
    ""
  end

  # Scan a while-body for any mutation of a local variable (by name).
  # Returns 1 if any mutating method call is found on the receiver
  # (push/pop/shift/unshift/<< / []= / delete / clear / insert /
  # replace / concat).  Used to block unsafe hoisting.
  def body_mutates_var?(body_nid, vname)
    if body_nid < 0
      return 0
    end
    t = @nd_type[body_nid]
    if t == "CallNode"
      mn = @nd_name[body_nid]
      recv = @nd_receiver[body_nid]
      if recv >= 0 && @nd_type[recv] == "LocalVariableReadNode" && @nd_name[recv] == vname
        if mn == "push" || mn == "pop" || mn == "shift" || mn == "unshift" ||
           mn == "<<" || mn == "[]=" || mn == "delete" || mn == "clear" ||
           mn == "insert" || mn == "replace" || mn == "concat" ||
           mn == "sort!" || mn == "reverse!" || mn == "compact!" || mn == "uniq!" ||
           mn == "merge!" || mn == "store" || mn == "update" || mn == "fill"
          return 1
        end
      end
    end
    if t == "LocalVariableWriteNode" && @nd_name[body_nid] == vname
      return 1
    end
    # Recurse into children
    if @nd_body[body_nid] >= 0
      if body_mutates_var?(@nd_body[body_nid], vname) == 1
        return 1
      end
    end
    stmts = parse_id_list(@nd_stmts[body_nid])
    k = 0
    while k < stmts.length
      if body_mutates_var?(stmts[k], vname) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_subsequent[body_nid] >= 0
      if body_mutates_var?(@nd_subsequent[body_nid], vname) == 1
        return 1
      end
    end
    if @nd_receiver[body_nid] >= 0
      if body_mutates_var?(@nd_receiver[body_nid], vname) == 1
        return 1
      end
    end
    args_id = @nd_arguments[body_nid]
    if args_id >= 0
      arr = get_args(args_id)
      k = 0
      while k < arr.length
        if body_mutates_var?(arr[k], vname) == 1
          return 1
        end
        k = k + 1
      end
    end
    0
  end

  # Return the local variable name on which .length/.size is called
  # inside a comparison predicate (for mutation scanning).  Empty if
  # the predicate doesn't match the hoist pattern.
  def hoist_receiver_var(pred_nid)
    if @nd_type[pred_nid] != "CallNode"
      return ""
    end
    op = @nd_name[pred_nid]
    if op != "<" && op != "<=" && op != ">" && op != ">="
      return ""
    end
    len_nid = -1
    args_id = @nd_arguments[pred_nid]
    if args_id >= 0
      a = get_args(args_id)
      if a.length > 0
        len_nid = a[0]
      end
    end
    if op == ">" || op == ">="
      len_nid = @nd_receiver[pred_nid]
    end
    if len_nid < 0 || @nd_type[len_nid] != "CallNode"
      return ""
    end
    mn = @nd_name[len_nid]
    if mn != "length" && mn != "size"
      return ""
    end
    recv = @nd_receiver[len_nid]
    if recv < 0 || @nd_type[recv] != "LocalVariableReadNode"
      return ""
    end
    @nd_name[recv]
  end

  # Check if while condition uses .length/.size and hoist if safe.
  # Supports string, arrays, and hashes.
  def try_hoist_strlen(pred_nid)
    if @nd_type[pred_nid] != "CallNode"
      return ""
    end
    op = @nd_name[pred_nid]
    if op != "<" && op != "<=" && op != ">"  && op != ">="
      return ""
    end
    # Find the .length/.size call
    len_nid = -1
    args_id = @nd_arguments[pred_nid]
    if args_id >= 0
      a = get_args(args_id)
      if a.length > 0
        len_nid = a[0]
      end
    end
    # For > or >=, the length call may be on the receiver side
    if op == ">" || op == ">="
      len_nid = @nd_receiver[pred_nid]
    end
    if len_nid < 0
      return ""
    end
    if @nd_type[len_nid] != "CallNode"
      return ""
    end
    mn = @nd_name[len_nid]
    if mn != "length" && mn != "size"
      return ""
    end
    recv = @nd_receiver[len_nid]
    if recv < 0
      return ""
    end
    rt = infer_type(recv)
    # Must be a local variable so we can check for mutations in the body
    if @nd_type[recv] != "LocalVariableReadNode"
      # string literal or ivar: be conservative, only hoist string (already safe)
      if rt != "string"
        return ""
      end
      tmp = new_temp
      rc = compile_expr_gc_rooted(recv)
      emit("  mrb_int " + tmp + " = sp_str_length(" + rc + ");")
      @hoisted_strlen_recv = rc
      return tmp
    end
    len_c = length_c_expr(rt, "")
    if len_c == ""
      return ""
    end
    # Check that the loop body doesn't mutate this variable
    vname = @nd_name[recv]
    # (The pred_nid is the while predicate; body scan happens below via caller
    #  passing @while_body_nid.  Here we only check if type is hoistable.)
    tmp = new_temp
    rc = compile_expr_gc_rooted(recv)
    emit("  mrb_int " + tmp + " = " + length_c_expr(rt, rc) + ";")
    @hoisted_strlen_recv = rc
    tmp
  end

  def compile_while_stmt(nid)
    old = @in_loop
    @in_loop = 1
    # Save outer hoist state to restore on exit (support nested loops)
    saved_var = @hoisted_strlen_var
    saved_recv = @hoisted_strlen_recv
    # Try to hoist length from condition (string/array/hash).  Skip if the
    # loop body mutates the receiver variable (push/pop/<< etc.).
    len_tmp = ""
    can_hoist = 1
    hoist_target = hoist_receiver_var(@nd_predicate[nid])
    if hoist_target != ""
      if body_mutates_var?(@nd_body[nid], hoist_target) == 1
        can_hoist = 0
      end
    end
    if can_hoist == 1
      len_tmp = try_hoist_strlen(@nd_predicate[nid])
      if len_tmp != ""
        @hoisted_strlen_var = len_tmp
      end
    end
    cond = compile_cond_expr(@nd_predicate[nid])
    emit("  while (" + cond + ") {")
    @indent = @indent + 1
    compile_stmts_body(@nd_body[nid])
    @indent = @indent - 1
    emit("  }")
    @hoisted_strlen_var = saved_var
    @hoisted_strlen_recv = saved_recv
    @in_loop = old
  end

  def compile_until_stmt(nid)
    old = @in_loop
    @in_loop = 1
    cond = compile_cond_expr(@nd_predicate[nid])
    emit("  while (!(" + cond + ")) {")
    @indent = @indent + 1
    compile_stmts_body(@nd_body[nid])
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_for_stmt(nid)
    old = @in_loop
    @in_loop = 1
    coll = @nd_collection[nid]
    if coll >= 0
      vname = "i"
      tgt = @nd_target[nid]
      if tgt >= 0
        if @nd_type[tgt] == "LocalVariableTargetNode"
          vname = @nd_name[tgt]
        end
      end
      if @nd_type[coll] == "RangeNode"
        left = compile_expr(@nd_left[coll])
        right = compile_expr(@nd_right[coll])
        cmp = range_excl_end(coll) == 1 ? "<" : "<="
        emit("  for (lv_" + vname + " = " + left + "; lv_" + vname + " " + cmp + " " + right + "; lv_" + vname + "++) {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[nid])
        @indent = @indent - 1
        emit("  }")
      else
        # for x in array
        ct = infer_type(coll)
        rc = compile_expr(coll)
        tmp = new_temp
        pfx = array_c_prefix(ct)
        emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_" + pfx + "_length(" + rc + "); " + tmp + "++) {")
        emit("    lv_" + vname + " = sp_" + pfx + "_get(" + rc + ", " + tmp + ");")

        @indent = @indent + 1
        compile_stmts_body(@nd_body[nid])
        @indent = @indent - 1
        emit("  }")
      end
    end
    @in_loop = old
  end

  def compile_case_stmt(nid)
    pred = @nd_predicate[nid]
    if pred < 0
      compile_case_no_pred(nid)
      return
    end
    pred_type = infer_type(pred)
    pred_val = compile_expr(pred)
    tmp = new_temp
    if pred_type == "string"
      emit("  const char *" + tmp + " = " + pred_val + ";")
    elsif is_obj_type(pred_type) == 1
      # `case obj when ClassName` — keep the temp as the right pointer
      # type so the when arms can read its NULLability and the static
      # class match in compile_when_conds picks the matching cls_id
      # path. Issue #67.
      bt = base_type(pred_type)
      obj_cname = bt[4, bt.length - 4]
      emit("  sp_" + obj_cname + " *" + tmp + " = " + pred_val + ";")
    else
      emit("  mrb_int " + tmp + " = " + pred_val + ";")
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      wid = conds[k]
      if @nd_type[wid] == "WhenNode"
        kw = "if"
        if k > 0
          kw = "} else if"
        end
        cond_str = compile_when_conds(wid, tmp, pred_type)
        emit("  " + kw + " (" + cond_str + ") {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[wid])
        @indent = @indent - 1
      end
      k = k + 1
    end
    ec = @nd_else_clause[nid]
    if ec >= 0
      emit("  } else {")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[ec])
      @indent = @indent - 1
    end
    emit("  }")
  end

  def compile_case_no_pred(nid)
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      wid = conds[k]
      if @nd_type[wid] == "WhenNode"
        kw = "if"
        if k > 0
          kw = "} else if"
        end
        wconds = parse_id_list(@nd_conditions[wid])
        cexpr = "0"
        if wconds.length > 0
          cexpr = compile_expr(wconds.first)
        end
        emit("  " + kw + " (" + cexpr + ") {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[wid])
        @indent = @indent - 1
      end
      k = k + 1
    end
    ec = @nd_else_clause[nid]
    if ec >= 0
      emit("  } else {")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[ec])
      @indent = @indent - 1
    end
    emit("  }")
  end

  def compile_when_conds(wid, tmp, pred_type)
    wconds = parse_id_list(@nd_conditions[wid])
    result = ""
    k = 0
    while k < wconds.length
      if k > 0
        result = result + " || "
      end
      cid = wconds[k]
      if @nd_type[cid] == "RangeNode"
        left = compile_expr(@nd_left[cid])
        right = compile_expr(@nd_right[cid])
        cmp = range_excl_end(cid) == 1 ? "<" : "<="
        result = result + "(" + tmp + " >= " + left + " && " + tmp + " " + cmp + " " + right + ")"
      elsif is_obj_type(pred_type) == 1 && @nd_type[cid] == "ConstantReadNode"
        # `case obj when ClassName` — resolve statically against the
        # predicate's known class. Predicate type `obj_X`:
        #   when X (or any ancestor of X)  → match (with a NULL guard
        #                                    when the predicate is
        #                                    nullable, since `nil` is
        #                                    not a class instance)
        #   when anything else             → no match
        # Subclass matching across an `obj_<Parent>` predicate that
        # actually carries an `obj_<Child>` instance needs a runtime
        # cls_id check; that's a separate enhancement (issue #67 only
        # covers the static-class form of the bug).
        cname = @nd_name[cid]
        if find_class_idx(cname) >= 0
          bt = base_type(pred_type)
          pred_cname = bt[4, bt.length - 4]
          if is_class_or_ancestor(pred_cname, cname) == 1
            if is_nullable_type(pred_type) == 1
              result = result + tmp + " != NULL"
            else
              result = result + "1"
            end
          else
            result = result + "0"
          end
        else
          result = result + "0"
        end
      else
        if pred_type == "string"
          result = result + "strcmp(" + tmp + ", " + compile_expr(cid) + ") == 0"
        else
          result = result + tmp + " == " + compile_expr(cid)
        end
      end
      k = k + 1
    end
    result
  end

  def compile_case_match_stmt(nid)
    pred = @nd_predicate[nid]
    pred_type = infer_type(pred)
    pred_val = compile_expr(pred)
    tmp = new_temp
    if pred_type == "poly"
      emit("  sp_RbVal " + tmp + " = " + pred_val + ";")
    else
      if pred_type == "string"
        emit("  const char *" + tmp + " = " + pred_val + ";")
      else
        if pred_type == "float"
          emit("  mrb_float " + tmp + " = " + pred_val + ";")
        else
          emit("  mrb_int " + tmp + " = " + pred_val + ";")
        end
      end
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      inid = conds[k]
      if @nd_type[inid] == "InNode"
        kw = "if"
        if k > 0
          kw = "} else if"
        end
        pat = @nd_pattern[inid]
        cond_str = compile_in_pattern(pat, tmp, pred_type)
        emit("  " + kw + " (" + cond_str + ") {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[inid])
        @indent = @indent - 1
      end
      k = k + 1
    end
    ec = @nd_else_clause[nid]
    if ec >= 0
      emit("  } else {")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[ec])
      @indent = @indent - 1
    end
    if conds.length > 0
      emit("  }")
    end
  end

  def compile_in_pattern(pat_id, tmp, pred_type)
    if pat_id < 0
      return "1"
    end
    pt = @nd_type[pat_id]
    if pt == "ConstantReadNode"
      cname = @nd_name[pat_id]
      if pred_type == "poly"
        if cname == "Integer"
          return tmp + ".tag == SP_TAG_INT"
        end
        if cname == "String"
          return tmp + ".tag == SP_TAG_STR"
        end
        if cname == "Float"
          return tmp + ".tag == SP_TAG_FLT"
        end
        return "0"
      end
      if cname == "Integer"
        return "1"
      end
      if cname == "String"
        return "1"
      end
      if cname == "Float"
        return "1"
      end
      return "0"
    end
    if pt == "IntegerNode"
      if pred_type == "poly"
        return "(" + tmp + ".tag == SP_TAG_INT && " + tmp + ".v.i == " + @nd_value[pat_id].to_s + ")"
      end
      return "#{tmp} == #{@nd_value[pat_id]}"
    end
    if pt == "StringNode"
      if pred_type == "poly"
        return "(" + tmp + ".tag == SP_TAG_STR && strcmp(" + tmp + ".v.s, " + c_string_literal(@nd_content[pat_id]) + ") == 0)"
      end
      return "strcmp(" + tmp + ", " + c_string_literal(@nd_content[pat_id]) + ") == 0"
    end
    if pt == "NilNode"
      if pred_type == "poly"
        return tmp + ".tag == SP_TAG_NIL"
      end
      return tmp + " == 0"
    end
    if pt == "TrueNode"
      if pred_type == "poly"
        return "(" + tmp + ".tag == SP_TAG_BOOL && " + tmp + ".v.b)"
      end
      return tmp + " != 0"
    end
    if pt == "FalseNode"
      if pred_type == "poly"
        return "(" + tmp + ".tag == SP_TAG_BOOL && !" + tmp + ".v.b)"
      end
      return tmp + " == 0"
    end
    if pt == "AlternationPatternNode"
      left = compile_in_pattern(@nd_left[pat_id], tmp, pred_type)
      right = compile_in_pattern(@nd_right[pat_id], tmp, pred_type)
      return "(" + left + " || " + right + ")"
    end
    "1"
  end

  def compile_return_stmt(nid)
    args_id = @nd_arguments[nid]
    if args_id >= 0
      arg_ids = get_args(args_id)
      if arg_ids.length > 1
        # `return a, b [, c]` — materialize as a fixed-arity tuple struct.
        @needs_gc = 1
        tt = tuple_type_from_elems(arg_ids)
        tname = tuple_c_name(tt)
        arr_tmp = new_temp
        emit("  " + tname + " *" + arr_tmp + " = (" + tname + " *)sp_gc_alloc(sizeof(" + tname + "), NULL, " + tuple_scan_name(tt) + ");")
        k = 0
        while k < arg_ids.length
          emit("  " + arr_tmp + "->_" + k.to_s + " = " + compile_expr(arg_ids[k]) + ";")
          k = k + 1
        end
        if @in_gc_scope == 1
          emit("  SP_GC_RESTORE();")
        end
        emit("  return " + arr_tmp + ";")
        return
      end
      if arg_ids.length > 0
        if @current_method_return == "poly"
          ret_expr = box_expr_to_poly(arg_ids[0])
          if @in_gc_scope == 1
            tmp = new_temp
            emit("  sp_RbVal " + tmp + " = " + ret_expr + ";")
            emit("  SP_GC_RESTORE();")
            emit("  return " + tmp + ";")
          else
            emit("  return " + ret_expr + ";")
          end
          return
        end
        rt = infer_type(arg_ids[0])
        # return nil in a nullable pointer method → return NULL
        if rt == "nil" && is_nullable_pointer_type(@current_method_return) == 1
          if @in_gc_scope == 1
            emit("  SP_GC_RESTORE();")
          end
          emit("  return NULL;")
          return
        end
        if @in_gc_scope == 1
          # Save return value, restore GC, then return
          tmp = new_temp
          emit("  " + c_type(rt) + " " + tmp + " = " + compile_expr(arg_ids[0]) + ";")
          emit("  SP_GC_RESTORE();")
          emit("  return " + tmp + ";")
        else
          emit("  return " + compile_expr(arg_ids[0]) + ";")
        end
        return
      end
    end
    # bare return — use NULL for nullable pointer types, 0 otherwise
    if @in_gc_scope == 1
      emit("  SP_GC_RESTORE();")
    end
    if is_nullable_pointer_type(@current_method_return) == 1
      emit("  return NULL;")
    else
      emit("  return 0;")
    end
  end


  def compile_call_stmt(nid)
    mname = @nd_name[nid]
    recv = @nd_receiver[nid]

    # define_method is handled at collection time, skip at runtime
    if mname == "define_method"
      return
    end

    # IO: puts, print, printf
    if compile_io_call_stmt(nid, mname, recv) == 1
      return
    end

    # File.open with block
    if compile_file_open_call_stmt(nid, mname, recv) == 1
      return
    end

    # Mutating operations: []=, delete, <<, replace, clear, push, reverse!, sort!
    if compile_mutating_call_stmt(nid, mname, recv) == 1
      return
    end

    # Block iteration: each, times, upto, downto, loop, reduce, inject, reject
    if compile_block_iteration_stmt(nid, mname, recv) == 1
      return
    end

    # Control flow: raise, system, trap, catch, throw, File ops, exit
    if compile_control_call_stmt(nid, mname, recv) == 1
      return
    end

    # attr_writer, map, select, yield method calls
    if compile_writer_and_block_call_stmt(nid, mname, recv) == 1
      return
    end

    # General
    val = compile_expr(nid)
    if val != "0"
      emit("  " + val + ";")
    end
  end

  def compile_io_call_stmt(nid, mname, recv)
    if mname == "puts"
      if recv < 0
        compile_puts(nid)
        return 1
      end
      if recv >= 0
        if @nd_type[recv] == "GlobalVariableReadNode"
          if @nd_name[recv] == "$stderr"
            compile_stderr_puts(nid)
            return 1
          end
        end
      end
    end
    if mname == "print"
      if recv < 0
        compile_print(nid)
        return 1
      end
    end
    if mname == "printf"
      if recv < 0
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length >= 1
            # First arg is format string
            fmt_expr = compile_expr(arg_ids[0])
            rest_args = ""
            k = 1
            while k < arg_ids.length
              at = infer_type(arg_ids[k])
              if at == "int"
                rest_args = rest_args + ", (int)" + compile_expr(arg_ids[k])
              else
                rest_args = rest_args + ", " + compile_expr(arg_ids[k])
              end
              k = k + 1
            end
            emit("  printf(" + fmt_expr + rest_args + ");")
            return 1
          end
        end
      end
    end
    0
  end

  def compile_file_open_call_stmt(nid, mname, recv)
    # File.open with block
    if mname == "open"
      if recv >= 0
        if @nd_type[recv] == "ConstantReadNode"
          if @nd_name[recv] == "File"
            if @nd_block[nid] >= 0
              args_id = @nd_arguments[nid]
              path_expr = "\"\""
              mode_expr = "\"r\""
              if args_id >= 0
                arg_ids = get_args(args_id)
                if arg_ids.length >= 1
                  path_expr = compile_expr(arg_ids[0])
                end
                if arg_ids.length >= 2
                  mode_expr = compile_expr(arg_ids[1])
                end
              end
              blk = @nd_block[nid]
              bp = get_block_param(nid, 0)
              ftmp = new_temp
              emit("  { FILE *" + ftmp + " = fopen(" + path_expr + ", " + mode_expr + ");")
              emit("  if (" + ftmp + ") {")
              # Compile block body -- f.puts => fprintf, f.each_line => fgets loop
              bbody = @nd_body[blk]
              if bbody >= 0
                bstmts = get_stmts(bbody)
                bk = 0
                while bk < bstmts.length
                  compile_file_block_stmt(bstmts[bk], ftmp, bp)
                  bk = bk + 1
                end
              end
              emit("  fclose(" + ftmp + ");")
              emit("  } }")
              return 1
            end
          end
        end
      end
    end
    0
  end

  def compile_mutating_call_stmt(nid, mname, recv)
    # []=
    if mname == "[]="
      if recv >= 0
        compile_bracket_assign(nid)
        return 1
      end
    end

    # store (Hash): equivalent to []=
    if mname == "store"
      if recv >= 0
        rt = infer_type(recv)
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length >= 2
            rc = compile_expr_gc_rooted(recv)
            val = compile_expr(aargs[1])
            if rt == "sym_int_hash"
              emit("  sp_SymIntHash_set(" + rc + ", " + compile_expr(aargs[0]) + ", " + val + ");")
              return 1
            end
            if rt == "sym_str_hash"
              emit("  sp_SymStrHash_set(" + rc + ", " + compile_expr(aargs[0]) + ", " + val + ");")
              return 1
            end
            if rt == "int_str_hash"
              emit("  sp_IntStrHash_set(" + rc + ", " + compile_expr(aargs[0]) + ", " + val + ");")
              return 1
            end
            key = compile_expr_as_string(aargs[0])
            if rt == "str_int_hash"
              emit("  sp_StrIntHash_set(" + rc + ", " + key + ", " + val + ");")
              return 1
            end
            if rt == "str_str_hash"
              emit("  sp_StrStrHash_set(" + rc + ", " + key + ", " + val + ");")
              return 1
            end
          end
        end
      end
    end

    # delete
    if mname == "delete"
      if recv >= 0
        rt = infer_type(recv)
        rc = compile_expr_gc_rooted(recv)
        if rt == "sym_int_hash"
          emit("  sp_SymIntHash_delete(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "sym_str_hash"
          emit("  sp_SymStrHash_delete(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "str_int_hash"
          emit("  sp_StrIntHash_delete(" + rc + ", " + compile_str_arg0(nid) + ");")
          return 1
        end
        if rt == "str_str_hash"
          emit("  sp_StrStrHash_delete(" + rc + ", " + compile_str_arg0(nid) + ");")
          return 1
        end
        if rt == "int_array"
          emit("  sp_IntArray_delete(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "str_array"
          emit("  sp_StrArray_delete(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
      end
    end

    # << on string (mutating append)
    if mname == "<<"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "mutable_str"
          @needs_mutable_str = 1
          rc = compile_expr_gc_rooted(recv)
          arg_id = @nd_arguments[nid]
          if arg_id >= 0
            argl = parse_id_list(@nd_args[arg_id])
            if argl.length > 0
              at = infer_type(argl[0])
              val = compile_expr(argl[0])
              if at == "int"
                emit("  sp_String_append(" + rc + ", sp_int_to_s(" + val + "));")
              else
                if at == "mutable_str"
                  emit("  sp_String_append(" + rc + ", " + val + "->data);")
                else
                  emit("  sp_String_append(" + rc + ", " + val + ");")
                end
              end
            end
          end
          return 1
        end
        if rt == "string"
          rc = compile_expr_gc_rooted(recv)
          val = compile_arg0(nid)
          # If receiver is a local variable, reassign
          if @nd_type[recv] == "LocalVariableReadNode"
            emit("  lv_" + @nd_name[recv] + " = sp_str_concat(lv_" + @nd_name[recv] + ", " + val + ");")
            return 1
          end
          if @nd_type[recv] == "InstanceVariableReadNode"
            emit("  " + self_arrow + sanitize_ivar(@nd_name[recv]) + " = sp_str_concat(self->" + sanitize_ivar(@nd_name[recv]) + ", " + val + ");")
            return 1
          end
        end
      end
    end

    # << on array (same as push)
    if mname == "<<"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "int_array" || rt == "sym_array"
          rc = compile_expr_gc_rooted(recv)
          av = compile_arg0(nid)
          a0id = -1
          args_id2 = @nd_arguments[nid]
          if args_id2 >= 0
            aargs2 = get_args(args_id2)
            if aargs2.length > 0
              a0id = aargs2[0]
            end
          end
          if a0id >= 0
            if infer_type(a0id) == "lambda"
              av = "(mrb_int)" + av
            end
          end
          emit("  sp_IntArray_push(" + rc + ", " + av + ");")
          return 1
        end
        if rt == "str_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_StrArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "float_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_FloatArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if is_ptr_array_type(rt) == 1
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_PtrArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
      end
    end

    # update / merge! on hash (mutating merge)
    if mname == "update" || mname == "merge!"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_int_hash"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_StrIntHash_update(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "str_str_hash"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_StrStrHash_update(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
      end
    end

    # concat on array (mutating append)
    if mname == "concat"
      if recv >= 0
        rt = infer_type(recv)
        if is_array_type(rt) == 1
          rc = compile_expr_gc_rooted(recv)
          arg = compile_arg0(nid)
          pfx = array_c_prefix(rt)
          tmp = new_temp
          emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_" + pfx + "_length(" + arg + "); " + tmp + "++)")
          emit("    sp_" + pfx + "_push(" + rc + ", sp_" + pfx + "_get(" + arg + ", " + tmp + "));")
          return 1
        end
        if rt == "mutable_str"
          @needs_mutable_str = 1
          rc = compile_expr_gc_rooted(recv)
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            ak = 0
            while ak < aargs.length
              emit("  sp_String_append(" + rc + ", " + compile_expr(aargs[ak]) + ");")
              ak = ak + 1
            end
          end
          return 1
        end
      end
    end

    # replace on string (mutating reassign)
    if mname == "replace"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "mutable_str"
          rc = compile_expr_gc_rooted(recv)
          val = compile_arg0(nid)
          emit("  " + rc + "->len = 0; " + rc + "->data[0] = 0; sp_String_append(" + rc + ", " + val + ");")
          return 1
        end
        if rt == "string"
          val = compile_arg0(nid)
          if @nd_type[recv] == "LocalVariableReadNode"
            emit("  lv_" + @nd_name[recv] + " = " + val + ";")
            return 1
          end
          if @nd_type[recv] == "InstanceVariableReadNode"
            emit("  " + self_arrow + sanitize_ivar(@nd_name[recv]) + " = " + val + ";")
            return 1
          end
        end
        if rt == "int_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_IntArray_replace(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "sym_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_IntArray_replace(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "str_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_StrArray_replace(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "float_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_FloatArray_replace(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
      end
    end

    # prepend on mutable_str (mutating prepend)
    if mname == "prepend"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "mutable_str"
          @needs_mutable_str = 1
          rc = compile_expr_gc_rooted(recv)
          val = compile_arg0(nid)
          emit("  sp_String_prepend(" + rc + ", " + val + ");")
          return 1
        end
      end
    end

    # clear on string (set to empty)
    if mname == "clear"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "mutable_str"
          rc = compile_expr_gc_rooted(recv)
          emit("  " + rc + "->len = 0; " + rc + "->data[0] = 0;")
          return 1
        end
        if rt == "string"
          if @nd_type[recv] == "LocalVariableReadNode"
            emit("  lv_" + @nd_name[recv] + " = \"\";")
            return 1
          end
          if @nd_type[recv] == "InstanceVariableReadNode"
            emit("  " + self_arrow + sanitize_ivar(@nd_name[recv]) + " = \"\";")
            return 1
          end
        end
      end
    end

    # push
    if mname == "push"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "int_array" || rt == "sym_array"
          rc = compile_expr_gc_rooted(recv)
          av = compile_arg0(nid)
          # If pushing a lambda value, cast to mrb_int
          a0id = -1
          args_id2 = @nd_arguments[nid]
          if args_id2 >= 0
            aargs2 = get_args(args_id2)
            if aargs2.length > 0
              a0id = aargs2[0]
            end
          end
          if a0id >= 0
            if infer_type(a0id) == "lambda"
              av = "(mrb_int)" + av
            end
          end
          emit("  sp_IntArray_push(" + rc + ", " + av + ");")
          return 1
        end
        if rt == "str_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_StrArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "float_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_FloatArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if is_ptr_array_type(rt) == 1
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_PtrArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
      end
    end

    # reverse! / sort!
    if mname == "reverse!"
      if recv >= 0
        rt = infer_type(recv)
        rev_pfx = ""
        if rt == "int_array" || rt == "sym_array"
          rev_pfx = "IntArray"
        end
        if rt == "str_array"
          rev_pfx = "StrArray"
        end
        if rt == "float_array"
          rev_pfx = "FloatArray"
        end
        if is_ptr_array_type(rt) == 1
          rev_pfx = "PtrArray"
        end
        if rev_pfx != ""
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_" + rev_pfx + "_reverse_bang(" + rc + ");")
          return 1
        end
      end
    end
    if mname == "sort!"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "int_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_IntArray_sort_bang(" + rc + ");")
          return 1
        end
        if rt == "sym_array"
          # sym sort compares by symbol name, not numeric ID
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_sym_array_sort(" + rc + ");")
          return 1
        end
        if rt == "str_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_StrArray_sort_bang(" + rc + ");")
          return 1
        end
        if rt == "float_array"
          rc = compile_expr_gc_rooted(recv)
          emit("  sp_FloatArray_sort_bang(" + rc + ");")
          return 1
        end
      end
    end
    0
  end

  def compile_block_iteration_stmt(nid, mname, recv)
    # each with block
    if mname == "each" || (mname == "each_pair" && recv >= 0)
      if @nd_block[nid] >= 0
        # For object types with yield-using each, use yield method call
        if recv >= 0
          ert = infer_type(recv)
          if is_obj_type(ert) == 1 && is_ptr_array_type(ert) == 0
            # Fall through to yield method handler below
          else
            compile_each_block(nid)
            return 1
          end
        else
          compile_each_block(nid)
          return 1
        end
      end
    end

    if mname == "each_with_index"
      if @nd_block[nid] >= 0
        compile_each_with_index_block(nid)
        return 1
      end
    end

    if mname == "each_slice"
      if @nd_block[nid] >= 0
        compile_each_slice_block(nid)
        return 1
      end
    end

    if mname == "each_char"
      if @nd_block[nid] >= 0 && recv >= 0
        rt = infer_type(recv)
        if rt == "string" || rt == "mutable_str"
          rc = compile_expr_gc_rooted(recv)
          bp = get_block_param(nid, 0)
          if bp == ""
            bp = "_c"
          end
          tmp = new_temp
          src = rc
          if rt == "mutable_str"
            src = rc + "->data"
          end
          src_tmp = new_temp
          cn_tmp = new_temp
          char_buf = new_temp
          emit("  const char *" + src_tmp + " = " + src + ";")
          emit("  for (mrb_int " + tmp + " = 0; " + src_tmp + "[" + tmp + "]; ) {")
          emit("    int " + cn_tmp + " = sp_utf8_advance(" + src_tmp + " + " + tmp + ");")
          emit("    char *" + char_buf + " = sp_str_alloc_raw(" + cn_tmp + " + 1);")
          emit("    memcpy(" + char_buf + ", " + src_tmp + " + " + tmp + ", " + cn_tmp + ");")
          emit("    " + char_buf + "[" + cn_tmp + "] = 0;")
          emit("    const char *lv_" + bp + " = " + char_buf + ";")
          @indent = @indent + 1
          push_scope
          declare_var(bp, "string")
          compile_stmts_body(@nd_body[@nd_block[nid]])
          pop_scope
          @indent = @indent - 1
          emit("    " + tmp + " += " + cn_tmp + ";")
          emit("  }")
          return 1
        end
      end
    end

    if mname == "each_byte"
      if @nd_block[nid] >= 0 && recv >= 0
        rt = infer_type(recv)
        if rt == "string" || rt == "mutable_str"
          rc = compile_expr_gc_rooted(recv)
          bp = get_block_param(nid, 0)
          if bp == ""
            bp = "_b"
          end
          tmp = new_temp
          src = rc
          if rt == "mutable_str"
            src = rc + "->data"
          end
          src_tmp = new_temp
          emit("  const char *" + src_tmp + " = " + src + ";")
          emit("  for (mrb_int " + tmp + " = 0; " + src_tmp + "[" + tmp + "]; " + tmp + "++) {")
          emit("    mrb_int lv_" + bp + " = (unsigned char)" + src_tmp + "[" + tmp + "];")
          @indent = @indent + 1
          push_scope
          declare_var(bp, "int")
          compile_stmts_body(@nd_body[@nd_block[nid]])
          pop_scope
          @indent = @indent - 1
          emit("  }")
          return 1
        end
      end
    end

    if mname == "each_line"
      if @nd_block[nid] >= 0 && recv >= 0
        rt = infer_type(recv)
        if rt == "string" || rt == "mutable_str"
          rc = compile_expr_gc_rooted(recv)
          bp = get_block_param(nid, 0)
          if bp == ""
            bp = "_l"
          end
          @needs_str_array = 1
          tmp_arr = new_temp
          tmp_i = new_temp
          src = rc
          if rt == "mutable_str"
            src = rc + "->data"
          end
          emit("  sp_StrArray *" + tmp_arr + " = sp_str_split(" + src + ", \"\\n\");")
          emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < " + tmp_arr + "->len; " + tmp_i + "++) {")
          emit("    const char *lv_" + bp + " = " + tmp_arr + "->data[" + tmp_i + "];")
          @indent = @indent + 1
          push_scope
          declare_var(bp, "string")
          compile_stmts_body(@nd_body[@nd_block[nid]])
          pop_scope
          @indent = @indent - 1
          emit("  }")
          return 1
        end
      end
    end

    if mname == "each_cons"
      if @nd_block[nid] >= 0
        compile_each_cons_block(nid)
        return 1
      end
    end

    if mname == "each_with_object"
      if @nd_block[nid] >= 0 && recv >= 0
        compile_each_with_object_block(nid)
        return 1
      end
    end

    if mname == "zip"
      if @nd_block[nid] >= 0 && recv >= 0
        old = @in_loop
        @in_loop = 1
        rt = infer_type(recv)
        rc = compile_expr_gc_rooted(recv)
        arg = compile_arg0(nid)
        bp1 = get_block_param(nid, 0)
        bp2 = get_block_param(nid, 1)
        if bp1 == ""
          bp1 = "_a"
        end
        if bp2 == ""
          bp2 = "_b"
        end
        pfx = array_c_prefix(rt)
        elem_t = elem_type_of_array(rt)
        et = c_type(elem_t)
        tmp_i = new_temp
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_" + pfx + "_length(" + rc + "); " + tmp_i + "++) {")
        emit("    " + et + " lv_" + bp1 + " = sp_" + pfx + "_get(" + rc + ", " + tmp_i + ");")
        emit("    " + et + " lv_" + bp2 + " = sp_" + pfx + "_get(" + arg + ", " + tmp_i + ");")
        @indent = @indent + 1
        push_scope
        declare_var(bp1, elem_t)
        declare_var(bp2, elem_t)
        compile_stmts_body(@nd_body[@nd_block[nid]])
        pop_scope
        @indent = @indent - 1
        emit("  }")
        @in_loop = old
        return 1
      end
    end

    if mname == "step"
      if @nd_block[nid] >= 0 && recv >= 0
        old = @in_loop
        @in_loop = 1
        rc = compile_expr_gc_rooted(recv)
        args_id = @nd_arguments[nid]
        limit_val = "0"
        step_val = "1"
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            limit_val = compile_expr(aargs[0])
          end
          if aargs.length > 1
            step_val = compile_expr(aargs[1])
          end
        end
        bp1 = get_block_param(nid, 0)
        synth = 0
        if bp1 == ""
          bp1 = "_i"
          synth = 1
        end
        # When the block omits its parameter, the synthesized `_i` isn't
        # declared anywhere — wrap the loop in a block scope and declare
        # it locally. This also avoids redefinition errors when multiple
        # paramless `step` blocks appear in the same function.
        if synth == 1
          emit("  {")
          emit("  mrb_int lv_" + bp1 + " = 0;")
        end
        emit("  for (lv_" + bp1 + " = " + rc + "; lv_" + bp1 + " <= " + limit_val + "; lv_" + bp1 + " += " + step_val + ") {")
        @indent = @indent + 1
        push_scope
        declare_var(bp1, "int")
        compile_stmts_body(@nd_body[@nd_block[nid]])
        pop_scope
        @indent = @indent - 1
        emit("  }")
        if synth == 1
          emit("  }")
        end
        @in_loop = old
        return 1
      end
    end

    if mname == "cycle"
      if @nd_block[nid] >= 0 && recv >= 0
        old = @in_loop
        @in_loop = 1
        rt = infer_type(recv)
        rc = compile_expr_gc_rooted(recv)
        n = compile_arg0(nid)
        bp1 = get_block_param(nid, 0)
        if bp1 == ""
          bp1 = "_x"
        end
        pfx = array_c_prefix(rt)
        et = elem_type_of_array(rt)
        tmp_c = new_temp
        tmp_i = new_temp
        emit("  for (mrb_int " + tmp_c + " = 0; " + tmp_c + " < " + n + "; " + tmp_c + "++)")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_" + pfx + "_length(" + rc + "); " + tmp_i + "++) {")
        emit("    " + c_type(et) + " lv_" + bp1 + " = sp_" + pfx + "_get(" + rc + ", " + tmp_i + ");")
        @indent = @indent + 1
        push_scope
        declare_var(bp1, et)
        compile_stmts_body(@nd_body[@nd_block[nid]])
        pop_scope
        @indent = @indent - 1
        emit("  }")
        @in_loop = old
        return 1
      end
    end

    # scan with block: str.scan(/re/) { |m| ... }
    if mname == "scan"
      if @nd_block[nid] >= 0
        recv = @nd_receiver[nid]
        if recv >= 0
          args_id = @nd_arguments[nid]
          if args_id >= 0
            argl = get_args(args_id)
            if argl.length > 0
              ridx = find_regexp_index(argl[0])
              if ridx >= 0
                @needs_str_array = 1
                rc = compile_expr_gc_rooted(recv)
                bp = get_block_param(nid, 0)
                if bp == ""
                  bp = "_m"
                end
                tmp_arr = new_temp
                tmp_i = new_temp
                emit("  sp_StrArray *" + tmp_arr + " = sp_re_scan(sp_re_pat_" + ridx.to_s + ", " + rc + ");")
                emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < " + tmp_arr + "->len; " + tmp_i + "++) {")
                emit("    const char *lv_" + bp + " = " + tmp_arr + "->data[" + tmp_i + "];")
                push_scope
                declare_var(bp, "string")
                blk = @nd_block[nid]
                if @nd_body[blk] >= 0
                  compile_stmts_body(@nd_body[blk])
                end
                pop_scope
                emit("  }")
                return 1
              end
            end
          end
        end
      end
    end

    # times/upto/downto with block
    if mname == "times"
      if @nd_block[nid] >= 0
        compile_times_block(nid)
        return 1
      end
    end
    if mname == "upto"
      if @nd_block[nid] >= 0
        compile_upto_block(nid)
        return 1
      end
    end
    if mname == "downto"
      if @nd_block[nid] >= 0
        compile_downto_block(nid)
        return 1
      end
    end

    # reduce/inject
    if mname == "reduce"
      if @nd_block[nid] >= 0
        compile_reduce_block(nid)
        return 1
      end
    end
    if mname == "inject"
      if @nd_block[nid] >= 0
        compile_reduce_block(nid)
        return 1
      end
    end

    # reject
    if mname == "reject"
      if @nd_block[nid] >= 0
        compile_reject_block(nid)
        return 1
      end
    end

    # loop
    if mname == "loop"
      if @nd_block[nid] >= 0
        old = @in_loop
        @in_loop = 1
        emit("  while (1) {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[@nd_block[nid]])
        @indent = @indent - 1
        emit("  }")
        @in_loop = old
        return 1
      end
    end
    0
  end

  def compile_control_call_stmt(nid, mname, recv)
    # raise
    if mname == "raise"
      if recv < 0
        @needs_setjmp = 1
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length >= 2
            # raise ClassName, "message" - use the message with class
            if @nd_type[arg_ids[0]] == "ConstantReadNode"
              emit("  sp_raise_cls(\"" + @nd_name[arg_ids[0]] + "\", " + compile_expr(arg_ids[1]) + ");")
            else
              emit("  sp_raise(" + compile_expr(arg_ids[1]) + ");")
            end
          else
            if arg_ids.length == 1
              # raise "message" or raise ClassName
              if @nd_type[arg_ids[0]] == "ConstantReadNode"
                emit("  sp_raise(\"" + @nd_name[arg_ids[0]] + "\");")
              else
                emit("  sp_raise(" + compile_expr(arg_ids[0]) + ");")
              end
            else
              emit("  sp_raise(\"RuntimeError\");")
            end
          end
        else
          emit("  sp_raise(\"RuntimeError\");")
        end
        return 1
      end
    end

    # system
    if mname == "system"
      if recv < 0
        emit("  fflush(stdout);")
        emit("  sp_last_status = system(" + compile_arg0(nid) + ");")
        return 1
      end
    end

    # trap (just ignore)
    if mname == "trap"
      if recv < 0
        return 1
      end
    end

    # catch/throw
    if mname == "catch"
      if recv < 0
        if @nd_block[nid] >= 0
          compile_catch_stmt(nid)
          return 1
        end
      end
    end
    if mname == "throw"
      if recv < 0
        compile_throw_stmt(nid)
        return 1
      end
    end

    # File operations
    if recv >= 0
      if @nd_type[recv] == "ConstantReadNode"
        rcname = @nd_name[recv]
        if rcname == "File"
          if mname == "write"
            args_id = @nd_arguments[nid]
            arg_ids = []
            if args_id >= 0
              arg_ids = get_args(args_id)
            end
            a0 = "0"
            a1 = "0"
            if arg_ids.length >= 1
              a0 = compile_expr(arg_ids[0])
            end
            if arg_ids.length >= 2
              a1 = compile_expr(arg_ids[1])
            end
            emit("  sp_file_write(" + a0 + ", " + a1 + ");")
            return 1
          end
          if mname == "delete"
            emit("  sp_file_delete(" + compile_arg0(nid) + ");")
            return 1
          end
        end
      end
    end

    # exit
    if mname == "exit"
      if recv < 0
        val = compile_arg0(nid)
        emit("  exit(" + val + ");")
        return 1
      end
    end

    # abort — print message to stderr and exit(1)
    if mname == "abort"
      if recv < 0
        args_id = @nd_arguments[nid]
        if args_id >= 0
          aargs = get_args(args_id)
          if aargs.length > 0
            msg = compile_expr(aargs[0])
            emit("  fputs(" + msg + ", stderr); fputc('\\n', stderr);")
          end
        end
        emit("  exit(1);")
        return 1
      end
    end
    0
  end

  def compile_writer_and_block_call_stmt(nid, mname, recv)
    # attr_writer: obj.x = val — only short-circuit to a direct field
    # write when `x=` is actually a registered attr_writer on the class
    # (or transitive parent). Otherwise fall through to method-call
    # dispatch so a real `def x=(v)` method gets called.
    if recv >= 0
      if mname.length > 1
        if mname[mname.length - 1] == "="
          bname = mname[0, mname.length - 1]
          rt = infer_type(recv)
          if is_obj_type(rt) == 1
            r_cname = rt[4, rt.length - 4]
            r_ci = find_class_idx(r_cname)
            is_writer = 0
            if r_ci >= 0
              is_writer = cls_has_attr_writer(r_ci, bname)
            end
            if is_writer == 1
              rc = compile_expr_gc_rooted(recv)
              arrow2 = "->"
              if is_value_type_obj(rt) == 1
                arrow2 = "."
              end
              emit("  " + rc + arrow2 + sanitize_ivar(bname) + " = " + compile_arg0(nid) + ";")
              return 1
            end
          end
        end
      end
    end

    # map with block
    if mname == "map"
      if @nd_block[nid] >= 0
        compile_map_block(nid)
        return 1
      end
    end

    # select with block
    if mname == "select"
      if @nd_block[nid] >= 0
        compile_select_block(nid)
        return 1
      end
    end

    # User-defined yield function called with block
    if @nd_block[nid] >= 0
      if recv < 0
        mi = find_method_idx(mname)
        if mi >= 0
          if @meth_has_yield[mi] == 1
            compile_yield_call_stmt(nid, mi)
            return 1
          end
        end
      end
      # Class method with yield
      if recv >= 0
        rtype = infer_type(recv)
        if is_obj_type(rtype) == 1
          cn = rtype[4, rtype.length - 4]
          cci = find_class_idx(cn)
          if cci >= 0
            midx = cls_find_method_direct(cci, mname)
            if midx >= 0
              if cls_method_has_yield(cci, midx) == 1
                compile_yield_method_call_stmt(nid, cci, midx, mname)
                return 1
              end
            end
            # Check parent
            if @cls_parents[cci] != ""
              pci = find_class_idx(@cls_parents[cci])
              if pci >= 0
                pidx = cls_find_method_direct(pci, mname)
                if pidx >= 0
                  if cls_method_has_yield(pci, pidx) == 1
                    compile_yield_method_call_stmt(nid, pci, pidx, mname)
                    return 1
                  end
                end
              end
            end
          end
        end
      end
    end
    0
  end


  def compile_file_block_stmt(nid, ftmp, bp)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "CallNode"
      mn = @nd_name[nid]
      r = @nd_receiver[nid]
      # f.puts "text"
      if mn == "puts"
        if r >= 0
          if @nd_type[r] == "LocalVariableReadNode"
            if @nd_name[r] == bp
              args_id = @nd_arguments[nid]
              if args_id >= 0
                arg_ids = get_args(args_id)
                if arg_ids.length >= 1
                  emit("  fprintf(" + ftmp + ", \"%s" + bsl_n + "\", " + compile_expr(arg_ids[0]) + ");")
                  return
                end
              end
              emit("  fputc('" + bsl_n + "', " + ftmp + ");")
              return
            end
          end
        end
      end
      # f.print "text"
      if mn == "print"
        if r >= 0
          if @nd_type[r] == "LocalVariableReadNode"
            if @nd_name[r] == bp
              args_id = @nd_arguments[nid]
              if args_id >= 0
                arg_ids = get_args(args_id)
                if arg_ids.length >= 1
                  emit("  fputs(" + compile_expr(arg_ids[0]) + ", " + ftmp + ");")
                  return
                end
              end
              return
            end
          end
        end
      end
      # f.write "text"
      if mn == "write"
        if r >= 0
          if @nd_type[r] == "LocalVariableReadNode"
            if @nd_name[r] == bp
              args_id = @nd_arguments[nid]
              if args_id >= 0
                arg_ids = get_args(args_id)
                if arg_ids.length >= 1
                  emit("  fputs(" + compile_expr(arg_ids[0]) + ", " + ftmp + ");")
                  return
                end
              end
              return
            end
          end
        end
      end
      # f.each_line { |line| ... }
      if mn == "each_line"
        if r >= 0
          if @nd_type[r] == "LocalVariableReadNode"
            if @nd_name[r] == bp
              if @nd_block[nid] >= 0
                lblk = @nd_block[nid]
                lbp = get_block_param(nid, 0)
                ltmp = new_temp
                emit("  { char " + ltmp + "[4096];")
                emit("  while (fgets(" + ltmp + ", sizeof(" + ltmp + "), " + ftmp + ")) {")
                emit("    const char *lv_" + lbp + " = " + ltmp + ";")
                push_scope
                declare_var(lbp, "string")
                # Compile block body
                lbbody = @nd_body[lblk]
                if lbbody >= 0
                  lbs = get_stmts(lbbody)
                  lbk = 0
                  while lbk < lbs.length
                    compile_stmt(lbs[lbk])
                    lbk = lbk + 1
                  end
                end
                pop_scope
                emit("  } }")
                return
              end
            end
          end
        end
      end
    end
    # Handle control flow: while/if/unless with file block context
    if t == "WhileNode"
      cond = @nd_predicate[nid]
      emit("  while (" + compile_expr(cond) + ") {")
      body = @nd_body[nid]
      if body >= 0
        bs = get_stmts(body)
        bk = 0
        while bk < bs.length
          compile_file_block_stmt(bs[bk], ftmp, bp)
          bk = bk + 1
        end
      end
      emit("  }")
      return
    end
    if t == "IfNode"
      cond = @nd_predicate[nid]
      emit("  if (" + compile_expr(cond) + ") {")
      body = @nd_body[nid]
      if body >= 0
        bs = get_stmts(body)
        bk = 0
        while bk < bs.length
          compile_file_block_stmt(bs[bk], ftmp, bp)
          bk = bk + 1
        end
      end
      if @nd_subsequent[nid] >= 0
        emit("  } else {")
        ebs = get_stmts(@nd_subsequent[nid])
        ebk = 0
        while ebk < ebs.length
          compile_file_block_stmt(ebs[ebk], ftmp, bp)
          ebk = ebk + 1
        end
      end
      emit("  }")
      return
    end
    # Fallback: compile as normal statement
    compile_stmt(nid)
  end

  def scan_lambda_free_vars(nid, params, locals, free_vars)
    # Scan AST node for free variable references
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "LocalVariableReadNode"
      vn = @nd_name[nid]
      if not_in(vn, params) == 1
        if not_in(vn, locals) == 1
          if not_in(vn, free_vars) == 1
            free_vars.push(vn)
          end
        end
      end
      return
    end
    if t == "LambdaNode"
      # For nested lambdas, find their free vars and add them to OUR free vars
      # (they need to be captured transitively)
      inner_pname = ""
      inner_params_id = @nd_parameters[nid]
      if inner_params_id >= 0
        reqs = parse_id_list(@nd_requireds[inner_params_id])
        if reqs.length > 0
          inner_pname = @nd_name[reqs[0]]
        end
      end
      inner_params = "".split(",")
      if inner_pname != ""
        inner_params.push(inner_pname)
      end
      inner_body = @nd_body[nid]
      if inner_body >= 0
        inner_locals = "".split(",")
        inner_free = "".split(",")
        scan_lambda_free_vars(inner_body, inner_params, inner_locals, inner_free)
        inner_free.each { |vn|
          if not_in(vn, params) == 1
            if not_in(vn, locals) == 1
              if not_in(vn, free_vars) == 1
                free_vars.push(vn)
              end
            end
          end
        }
      end
      return
    end
    # Collect local writes — scan expression first to detect reads before the write
    if t == "LocalVariableWriteNode"
      vn = @nd_name[nid]
      # Scan the RHS expression first (may contain reads of the same var)
      if @nd_expression[nid] >= 0
        scan_lambda_free_vars(@nd_expression[nid], params, locals, free_vars)
      end
      if not_in(vn, locals) == 1
        if not_in(vn, params) == 1
          if not_in(vn, free_vars) == 1
            # Check if variable exists in outer scope — if so, it's a free var
            outer_t = find_var_type(vn)
            if outer_t != ""
              free_vars.push(vn)
            else
              locals.push(vn)
            end
          end
        end
      end
      return
    end
    # Recurse
    if @nd_body[nid] >= 0
      scan_lambda_free_vars(@nd_body[nid], params, locals, free_vars)
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      scan_lambda_free_vars(stmts[k], params, locals, free_vars)
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      scan_lambda_free_vars(@nd_expression[nid], params, locals, free_vars)
    end
    if @nd_left[nid] >= 0
      scan_lambda_free_vars(@nd_left[nid], params, locals, free_vars)
    end
    if @nd_right[nid] >= 0
      scan_lambda_free_vars(@nd_right[nid], params, locals, free_vars)
    end
    if @nd_receiver[nid] >= 0
      scan_lambda_free_vars(@nd_receiver[nid], params, locals, free_vars)
    end
    if @nd_arguments[nid] >= 0
      scan_lambda_free_vars(@nd_arguments[nid], params, locals, free_vars)
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      scan_lambda_free_vars(args[k], params, locals, free_vars)
      k = k + 1
    end
    if @nd_predicate[nid] >= 0
      scan_lambda_free_vars(@nd_predicate[nid], params, locals, free_vars)
    end
    if @nd_subsequent[nid] >= 0
      scan_lambda_free_vars(@nd_subsequent[nid], params, locals, free_vars)
    end
    if @nd_else_clause[nid] >= 0
      scan_lambda_free_vars(@nd_else_clause[nid], params, locals, free_vars)
    end
    if @nd_block[nid] >= 0
      scan_lambda_free_vars(@nd_block[nid], params, locals, free_vars)
    end
    elems = parse_id_list(@nd_elements[nid])
    k = 0
    while k < elems.length
      scan_lambda_free_vars(elems[k], params, locals, free_vars)
      k = k + 1
    end
    if @nd_collection[nid] >= 0
      scan_lambda_free_vars(@nd_collection[nid], params, locals, free_vars)
    end
  end

  def scan_lambda_ret_types(stmts)
    stmts.each { |sid|
      scan_lambda_ret_types_node(sid)
    }
    # Also check: h = method_call() where method returns lambda
    stmts.each { |sid|
      if @nd_type[sid] == "LocalVariableWriteNode"
        vn = @nd_name[sid]
        expr = @nd_expression[sid]
        if expr >= 0 && @nd_type[expr] == "CallNode"
          call_ret = infer_type(expr)
          if call_ret == "lambda"
            # Find the method and its lambda return
            mn = @nd_name[expr]
            mi = find_method_idx(mn)
            if mi >= 0
              bid = @meth_body_ids[mi]
              if bid >= 0
                mbs = get_stmts(bid)
                if mbs.length > 0
                  last = mbs.last
                  if @nd_type[last] == "LambdaNode"
                    lbody = @nd_body[last]
                    if lbody >= 0
                      lbs = get_stmts(lbody)
                      if lbs.length > 0
                        lrt = infer_type(lbs.last)
                        if not_in(vn, @lambda_var_ret_names) == 1
                          @lambda_var_ret_names.push(vn)
                          @lambda_var_ret_types.push(lrt)
                        end
                      end
                    end
                  end
                end
              end
            end
          end
        end
      end
    }
  end

  def scan_lambda_ret_types_node(nid)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "LocalVariableWriteNode"
      vn = @nd_name[nid]
      expr = @nd_expression[nid]
      if expr >= 0 && @nd_type[expr] == "LambdaNode"
        lbody = @nd_body[expr]
        if lbody >= 0
          lbs = get_stmts(lbody)
          if lbs.length > 0
            lrt = infer_type(lbs.last)
            if not_in(vn, @lambda_var_ret_names) == 1
              @lambda_var_ret_names.push(vn)
              @lambda_var_ret_types.push(lrt)
            end
          end
        end
      end
      scan_lambda_ret_types_node(expr)
    end
    # Scan into method bodies to find lambdas returned from methods
    if t == "DefNode"
      if @nd_body[nid] >= 0
        scan_lambda_ret_types_node(@nd_body[nid])
      end
      return
    end
    if @nd_body[nid] >= 0
      scan_lambda_ret_types_node(@nd_body[nid])
    end
    if @nd_expression[nid] >= 0
      scan_lambda_ret_types_node(@nd_expression[nid])
    end
    ss = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < ss.length
      scan_lambda_ret_types_node(ss[k])
      k = k + 1
    end
  end

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

  def lam_box(expr, vtype)
    bt = base_type(vtype)
    if bt == "string"
      return "sp_lam_int((mrb_int)" + expr + ")"
    end
    if bt == "float"
      return "sp_lam_int(*(mrb_int*)&(mrb_float){" + expr + "})"
    end
    if bt == "bool"
      return "sp_lam_bool(" + expr + ")"
    end
    if bt == "nil" || bt == "void"
      return "&sp_lam_nil_val"
    end
    "sp_lam_int(" + expr + ")"
  end

  def lam_unbox(expr, vtype)
    bt = base_type(vtype)
    if bt == "string"
      return "(const char*)sp_lam_to_int(" + expr + ")"
    end
    if bt == "float"
      return "*(mrb_float*)&(mrb_int){sp_lam_to_int(" + expr + ")}"
    end
    if bt == "bool"
      return "(" + expr + ")->u.bval"
    end
    if bt == "nil" || bt == "void"
      return "(sp_lam_to_int(" + expr + "), 0)"
    end
    "sp_lam_to_int(" + expr + ")"
  end

  def compile_lambda_body_expr(nid, params, captures)
    # Compile an expression inside a lambda body, replacing:
    # - param refs with lv_param (local)
    # - captured var refs with self->captures[i]
    if nid < 0
      return "&sp_lam_nil_val"
    end
    t = @nd_type[nid]
    if t == "LocalVariableReadNode"
      vn = @nd_name[nid]
      # Check param
      if not_in(vn, params) == 0
        return "lv_" + vn
      end
      # Check captures
      ci = 0
      while ci < captures.length
        if captures[ci] == vn
          ct = ""
          if ci < @lambda_capture_cell_types.length
            ct = @lambda_capture_cell_types[ci]
          end
          if ct != ""
            # Typed cell capture: dereference and box
            deref = "*(" + c_type(ct) + "*)self->captures[" + ci.to_s + "]"
            return lam_box(deref, ct)
          end
          return "self->captures[" + ci.to_s + "]"
        end
        ci = ci + 1
      end
      return "lv_" + vn
    end
    if t == "LocalVariableWriteNode"
      vn = @nd_name[nid]
      val = compile_lambda_body_expr(@nd_expression[nid], params, captures)
      # Check if capture
      ci = 0
      while ci < captures.length
        if captures[ci] == vn
          ct = ""
          if ci < @lambda_capture_cell_types.length
            ct = @lambda_capture_cell_types[ci]
          end
          if ct != ""
            # Typed cell capture: unbox and store
            cptr = "*(" + c_type(ct) + "*)self->captures[" + ci.to_s + "]"
            return "(" + cptr + " = " + lam_unbox(val, ct) + ", " + val + ")"
          end
          return "(self->captures[" + ci.to_s + "] = " + val + ")"
        end
        ci = ci + 1
      end
      emit("  sp_Val *lv_" + vn + " = " + val + ";")
      return "lv_" + vn
    end
    if t == "IntegerNode"
      return "sp_lam_int(" + @nd_value[nid].to_s + ")"
    end
    if t == "TrueNode"
      return "sp_lam_bool(TRUE)"
    end
    if t == "FalseNode"
      return "sp_lam_bool(FALSE)"
    end
    if t == "NilNode"
      return "&sp_lam_nil_val"
    end
    if t == "StringNode"
      return "sp_lam_int(0)"
    end
    if t == "ConstantReadNode"
      cname = @nd_name[nid]
      ci = find_const_idx(cname)
      if ci >= 0
        return "cst_" + cname
      end
      return "&sp_lam_nil_val"
    end
    if t == "LambdaNode"
      return compile_lambda_expr(nid)
    end
    if t == "CallNode"
      mname = @nd_name[nid]
      recv = @nd_receiver[nid]
      # f[arg] -> sp_lam_call(f, arg)
      if mname == "[]"
        if recv >= 0
          rc = compile_lambda_body_expr(recv, params, captures)
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length > 0
              ac = compile_lambda_body_expr(aargs.first, params, captures)
              return "sp_lam_call(" + rc + ", " + ac + ")"
            end
          end
          return "sp_lam_call(" + rc + ", &sp_lam_nil_val)"
        end
      end
      # f.call(arg) -> sp_lam_call(f, arg)
      if mname == "call"
        if recv >= 0
          rc = compile_lambda_body_expr(recv, params, captures)
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            if aargs.length > 0
              ac = compile_lambda_body_expr(aargs.first, params, captures)
              return "sp_lam_call(" + rc + ", " + ac + ")"
            end
          end
          return "sp_lam_call(" + rc + ", &sp_lam_nil_val)"
        end
      end
      # No receiver: bare function call
      if recv < 0
        if mname == "+"
          return "sp_lam_int(0)"
        end
        mi = find_method_idx(mname)
        if mi >= 0
          ca = ""
          args_id = @nd_arguments[nid]
          if args_id >= 0
            aargs = get_args(args_id)
            k = 0
            while k < aargs.length
              if k > 0
                ca = ca + ", "
              end
              ca = ca + compile_lambda_body_expr(aargs[k], params, captures)
              k = k + 1
            end
          end
          return "sp_" + sanitize_name(mname) + "(" + ca + ")"
        end
      end
      # Arithmetic on sp_Val (+ operator)
      if mname == "+"
        if recv >= 0
          rc = compile_lambda_body_expr(recv, params, captures)
          ac = compile_lambda_body_expr(get_args(@nd_arguments[nid])[0], params, captures)
          return "sp_lam_int(sp_lam_to_int(" + rc + ") + sp_lam_to_int(" + ac + "))"
        end
      end
      return "&sp_lam_nil_val"
    end
    if t == "IfNode"
      pred = compile_lambda_body_expr(@nd_predicate[nid], params, captures)
      body = @nd_body[nid]
      bexpr = "&sp_lam_nil_val"
      if body >= 0
        bs = get_stmts(body)
        if bs.length > 0
          bexpr = compile_lambda_body_expr(bs.last, params, captures)
        end
      end
      ec = @nd_else_clause[nid]
      eexpr = "&sp_lam_nil_val"
      if ec >= 0
        ebs = get_stmts(@nd_body[ec])
        if ebs.length > 0
          eexpr = compile_lambda_body_expr(ebs[ebs.length - 1], params, captures)
        end
      end
      return "(" + pred + " ? " + bexpr + " : " + eexpr + ")"
    end
    "&sp_lam_nil_val"
  end

  def wrap_as_sp_val(nid)
    at = infer_type(nid)
    if at == "lambda"
      return compile_expr(nid)
    end
    if at == "int"
      return "sp_lam_int(" + compile_expr(nid) + ")"
    end
    if at == "bool"
      return "sp_lam_bool(" + compile_expr(nid) + ")"
    end
    if @nd_type[nid] == "NilNode"
      return "&sp_lam_nil_val"
    end
    # Default: try to compile as expression
    compile_expr(nid)
  end

  def compile_lambda_expr(nid)
    @needs_lambda = 1
    # Get the parameter name
    pname = ""
    params_id = @nd_parameters[nid]
    if params_id >= 0
      reqs = parse_id_list(@nd_requireds[params_id])
      if reqs.length > 0
        pname = @nd_name[reqs[0]]
      end
    end
    param_arr = "".split(",")
    if pname != ""
      param_arr.push(pname)
    end

    body = @nd_body[nid]
    # Find free variables (captures)
    free_vars = "".split(",")
    locals = "".split(",")
    if body >= 0
      scan_lambda_free_vars(body, param_arr, locals, free_vars)
    end

    # Generate lambda function
    lam_id = @lambda_counter
    @lambda_counter = @lambda_counter + 1
    fname = "_lam_" + lam_id.to_s

    # Compute capture cell types (before body compilation)
    cap_cell_types = "".split(",")
    k = 0
    while k < free_vars.length
      fv = free_vars[k]
      cell = heap_promoted_cell(fv)
      if cell == "" && @in_fiber_body == 1 && fiber_capture_index(fv) >= 0
        cell = "_cap->" + fv
      end
      # Regular locals will be heap-promoted (not lambda params/captures)
      if cell == "" && not_in(fv, @lambda_params) == 1
        is_enclosing_cap = 0
        ci = 0
        while ci < @lambda_captures.length
          if @lambda_captures[ci] == fv
            is_enclosing_cap = 1
          end
          ci = ci + 1
        end
        if is_enclosing_cap == 0
          vt = find_var_type(fv)
          if vt != "" && vt != "lambda"
            cell = "will_promote"
          end
        end
      end
      if cell != ""
        cap_cell_types.push(find_var_type(fv))
      else
        cap_cell_types.push("")
      end
      k = k + 1
    end

    # Check if we have typed cell captures (need regular compiler path)
    has_typed_caps = 0
    k = 0
    while k < cap_cell_types.length
      if cap_cell_types[k] != ""
        has_typed_caps = 1
      end
      k = k + 1
    end

    # Get body expression
    bexpr = "&sp_lam_nil_val"
    if body >= 0
      bs = get_stmts(body)
      if bs.length > 0 && has_typed_caps == 1
        # Typed captures: compile body using regular compiler
        save_out = @out_lines
        save_indent = @indent
        save_hp_names_len = @heap_promoted_names.length
        save_hp_cells_len = @heap_promoted_cells.length
        @out_lines = "".split(",")
        @indent = 1

        push_scope
        # Declare parameter with proper type
        if pname != ""
          ptype = infer_type(bs.last)
          if ptype == ""
            ptype = "int"
          end
          declare_var(pname, "int")
        end
        # Set up cell pointer locals for typed captures
        k = 0
        while k < free_vars.length
          if cap_cell_types[k] != ""
            ct = c_type(cap_cell_types[k])
            cell_local = "_lc_" + free_vars[k]
            emit("  " + ct + " *" + cell_local + " = (" + ct + "*)self->captures[" + k.to_s + "];")
            @heap_promoted_names.push(free_vars[k])
            @heap_promoted_cells.push(cell_local)
            declare_var(free_vars[k], cap_cell_types[k])
          end
          k = k + 1
        end
        # Declare body-local variables
        lnames = "".split(",")
        ltypes = "".split(",")
        excl = "".split(",")
        if pname != ""
          excl.push(pname)
        end
        k = 0
        while k < free_vars.length
          excl.push(free_vars[k])
          k = k + 1
        end
        scan_locals(body, lnames, ltypes, excl)
        lk = 0
        while lk < lnames.length
          declare_var(lnames[lk], ltypes[lk])
          emit("  " + c_type(ltypes[lk]) + " lv_" + lnames[lk] + " = " + c_default_val(ltypes[lk]) + ";")
          lk = lk + 1
        end
        # Compile body statements
        i = 0
        while i < bs.length - 1
          compile_stmt(bs[i])
          i = i + 1
        end
        last = bs.last
        last_type = infer_type(last)
        if @nd_type[last] == "LocalVariableWriteNode" || @nd_type[last] == "LocalVariableOperatorWriteNode"
          compile_stmt(last)
        end
        last_val = compile_expr(last)

        pop_scope
        body_stmts = @out_lines.join(10.chr) + 10.chr
        @out_lines = save_out
        @indent = save_indent
        # Restore heap promoted
        while @heap_promoted_names.length > save_hp_names_len
          @heap_promoted_names.pop
        end
        while @heap_promoted_cells.length > save_hp_cells_len
          @heap_promoted_cells.pop
        end

        # Build lambda function with typed body
        @lambda_funcs <<"static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) {\n"
        if pname != ""
          @lambda_funcs << "  mrb_int lv_"
          @lambda_funcs << pname
          @lambda_funcs << " = sp_lam_to_int(arg);\n"
        end
        @lambda_funcs << body_stmts
        @lambda_funcs << 10.chr
        bexpr = lam_box(last_val, last_type)
        @lambda_funcs << "  return "
        @lambda_funcs << bexpr
        @lambda_funcs << ";\n}\n\n"
      elsif bs.length > 0
        # No typed captures: use sp_Val* lambda body compiler
        save_out = @out_lines
        save_params = @lambda_params
        save_captures = @lambda_captures
        save_cell_types = @lambda_capture_cell_types
        @out_lines = "".split(",")
        @lambda_params = param_arr
        @lambda_captures = free_vars
        @lambda_capture_cell_types = cap_cell_types
        si = 0
        while si < bs.length - 1
          side_expr = compile_lambda_body_expr(bs[si], param_arr, free_vars)
          emit("  " + side_expr + ";")
          si = si + 1
        end
        bexpr = compile_lambda_body_expr(bs.last, param_arr, free_vars)
        body_stmts = @out_lines.join(10.chr) + 10.chr
        @out_lines = save_out
        @lambda_params = save_params
        @lambda_captures = save_captures
        @lambda_capture_cell_types = save_cell_types

        if body_stmts != ""
          @lambda_funcs <<"static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) {\n"
          if pname != ""
            @lambda_funcs <<"  sp_Val *lv_" + pname + " = arg;\n"
          end
          @lambda_funcs <<"  (void)self;\n"
          @lambda_funcs <<body_stmts + 10.chr
          @lambda_funcs <<"  return " + bexpr + ";\n"
          @lambda_funcs <<"}\n\n"
        else
          @lambda_funcs <<"static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) {\n"
          if pname != ""
            @lambda_funcs <<"  sp_Val *lv_" + pname + " = arg;\n"
          end
          @lambda_funcs <<"  (void)self;\n"
          @lambda_funcs <<"  return " + bexpr + ";\n"
          @lambda_funcs <<"}\n\n"
        end
      else
        @lambda_funcs <<"static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) { (void)self; (void)arg; return &sp_lam_nil_val; }\n\n"
      end
    else
      @lambda_funcs <<"static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) { (void)self; (void)arg; return &sp_lam_nil_val; }\n\n"
    end

    # Build the closure creation expression
    if free_vars.length > 0
      # Heap-promote regular locals that need cell captures
      k = 0
      while k < free_vars.length
        if cap_cell_types[k] != "" && heap_promoted_cell(free_vars[k]) == ""
          fv = free_vars[k]
          # Skip if it's a fiber capture (already has cell) or lambda context
          if @in_fiber_body == 0 || fiber_capture_index(fv) < 0
            if not_in(fv, @heap_promoted_names) == 1
              ct = c_type(cap_cell_types[k])
              cell = "_hcell_" + fv + "_l" + lam_id.to_s
              emit("  " + ct + " *" + cell + " = (" + ct + "*)sp_gc_alloc(sizeof(" + ct + "), NULL, NULL);")
              emit("  *" + cell + " = " + fiber_var_ref(fv) + ";")
              @heap_promoted_names.push(fv)
              @heap_promoted_cells.push(cell)
            end
          end
        end
        k = k + 1
      end
      tmp = new_temp
      emit("  sp_Val *" + tmp + " = sp_lam_proc(" + fname + ", " + free_vars.length.to_s + ");")
      k = 0
      while k < free_vars.length
        fv = free_vars[k]
        if cap_cell_types[k] != ""
          # Heap-promoted or fiber-captured: store cell pointer cast to sp_Val*
          cell = heap_promoted_cell(fv)
          if cell == "" && @in_fiber_body == 1 && fiber_capture_index(fv) >= 0
            cell = "_cap->" + fv
          end
          emit("  " + tmp + "->captures[" + k.to_s + "] = (sp_Val*)" + cell + ";")
        else
          # Check if it's a param or capture of enclosing lambda (sp_Val* world)
          is_lam_ctx = 0
          if not_in(fv, @lambda_params) == 0
            is_lam_ctx = 1
            emit("  " + tmp + "->captures[" + k.to_s + "] = lv_" + fv + ";")
          else
            ci = 0
            while ci < @lambda_captures.length
              if @lambda_captures[ci] == fv
                is_lam_ctx = 1
                emit("  " + tmp + "->captures[" + k.to_s + "] = self->captures[" + ci.to_s + "];")
              end
              ci = ci + 1
            end
          end
          if is_lam_ctx == 0
            # Regular local: should have been heap-promoted already
            cell = heap_promoted_cell(fv)
            if cell != ""
              emit("  " + tmp + "->captures[" + k.to_s + "] = (sp_Val*)" + cell + ";")
            else
              emit("  " + tmp + "->captures[" + k.to_s + "] = " + fiber_var_ref(fv) + ";")
            end
          end
        end
        k = k + 1
      end
      return tmp
    else
      return "sp_lam_proc(" + fname + ", 0)"
    end
  end

  # Build the proc-fn body prelude that unpacks the args array passed
  # to the uniform `(void *_cap, mrb_int *args)` signature into named
  # `lv_<bp>` locals — one `mrb_int lv_<bp> = args[<idx>];` line per
  # block param. Used at both proc-fn body emit sites (captures and
  # no-captures branches; identical shape).
  def proc_fn_args_unpack(bps)
    s = ""
    bk = 0
    while bk < bps.length
      s = s + "  mrb_int lv_" + bps[bk] + " = args[" + bk.to_s + "];\n"
      bk = bk + 1
    end
    s
  end

  def compile_proc_literal(nid)
    blk = @nd_block[nid]
    if blk < 0
      return "sp_proc_new(NULL, NULL, NULL)"
    end
    # Collect every block param name. Single-param blocks fall through
    # to the existing `_unused` fallback for parameterless bodies.
    bps = "".split(",")
    pi = 0
    pn = get_block_param(nid, pi)
    while pn != ""
      bps.push(pn)
      pi = pi + 1
      pn = get_block_param(nid, pi)
    end
    if bps.length == 0
      bps.push("_unused")
    end
    # Generate a static function for the proc body
    @proc_counter = @proc_counter + 1
    pid = @proc_counter
    fname = "_sp_proc_fn_" + pid.to_s
    cap_name = "_proc_cap_" + pid.to_s
    cap_scan_name = "_proc_cap_scan_" + pid.to_s
    bbody = @nd_body[blk]

    # Detect captures (free variables that resolve in outer scope).
    # Every block param is in scope inside the body; only locals from
    # the outer scope read inside the body count as free.
    free_vars = "".split(",")
    if bbody >= 0
      # scan_lambda_free_vars treats `params` as read-only, so bps can
      # be passed directly without an intermediate copy.
      proc_locals = "".split(",")
      scan_lambda_free_vars(bbody, bps, proc_locals, free_vars)
    end
    captures = "".split(",")
    capture_types = "".split(",")
    fk = 0
    while fk < free_vars.length
      fv = free_vars[fk]
      vt = find_var_type(fv)
      if vt != ""
        captures.push(fv)
        capture_types.push(vt)
      end
      fk = fk + 1
    end
    has_captures = 0
    if captures.length > 0
      has_captures = 1
    end

    # Compile the body. While captures > 0, set @in_proc_body so that
    # compile_expr's fiber_var_ref rewrites captured-var reads/writes to
    # (*_cap->VN). Heap promotion of those vars in the enclosing scope is
    # done after body compile (so cells exist before they're referenced
    # in the cap struct allocation).
    save_out = @out_lines
    @out_lines = "".split(",")
    saved_in_proc_body = @in_proc_body
    saved_proc_captures = @proc_captures
    saved_proc_capture_types = @proc_capture_types
    if has_captures == 1
      @in_proc_body = 1
      @proc_captures = captures
      @proc_capture_types = capture_types
    end
    push_scope
    di = 0
    while di < bps.length
      declare_var(bps[di], "int")
      di = di + 1
    end
    bexpr = "0"
    body_stmts = ""
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        k = 0
        while k < bs.length
          lt = infer_type(bs[k])
          if k == bs.length - 1
            last_t = @nd_type[bs[k]]
            if last_t == "LocalVariableWriteNode" || last_t == "LocalVariableOperatorWriteNode"
              compile_stmt(bs[k])
              body_stmts = @out_lines.join(10.chr) + 10.chr
              @out_lines = "".split(",")
              bexpr = fiber_var_ref(@nd_name[bs[k]])
            elsif lt != "void"
              body_stmts = @out_lines.join(10.chr) + 10.chr
              @out_lines = "".split(",")
              bexpr = compile_expr(bs[k])
              extra = @out_lines.join(10.chr) + 10.chr
              @out_lines = "".split(",")
              body_stmts = body_stmts + extra
            else
              compile_stmt(bs[k])
            end
          else
            compile_stmt(bs[k])
          end
          k = k + 1
        end
        if body_stmts == ""
          body_stmts = @out_lines.join(10.chr) + 10.chr
          @out_lines = "".split(",")
        end
      end
    end
    pop_scope
    @in_proc_body = saved_in_proc_body
    @proc_captures = saved_proc_captures
    @proc_capture_types = saved_proc_capture_types
    @out_lines = save_out

    if has_captures == 1
      # Emit per-proc capture struct + scan function + body fn that casts
      # the void* cap to the typed struct.
      @lambda_funcs << "typedef struct { "
      k = 0
      while k < captures.length
        @lambda_funcs << c_type(capture_types[k])
        @lambda_funcs << " *"
        @lambda_funcs << captures[k]
        @lambda_funcs << "; "
        k = k + 1
      end
      @lambda_funcs << "} "
      @lambda_funcs << cap_name
      @lambda_funcs << ";\n"
      @lambda_funcs << "static void "
      @lambda_funcs << cap_scan_name
      @lambda_funcs << "(void *p) {\n"
      @lambda_funcs << "  "
      @lambda_funcs << cap_name
      @lambda_funcs << " *_c = ("
      @lambda_funcs << cap_name
      @lambda_funcs << " *)p;\n"
      k = 0
      while k < captures.length
        @lambda_funcs << "  if (_c->"
        @lambda_funcs << captures[k]
        @lambda_funcs << ") sp_gc_mark((void *)_c->"
        @lambda_funcs << captures[k]
        @lambda_funcs << ");\n"
        k = k + 1
      end
      @lambda_funcs << "}\n"
      @lambda_funcs << "static mrb_int "
      @lambda_funcs << fname
      @lambda_funcs << "(void *_cap_raw, mrb_int *args) {\n"
      @lambda_funcs << proc_fn_args_unpack(bps)
      @lambda_funcs << "  "
      @lambda_funcs << cap_name
      @lambda_funcs << " *_cap = ("
      @lambda_funcs << cap_name
      @lambda_funcs << " *)_cap_raw;\n"
      if body_stmts != ""
        @lambda_funcs << body_stmts
      end
      @lambda_funcs << "  return "
      @lambda_funcs << bexpr
      @lambda_funcs << ";\n}\n"

      # In the enclosing scope: heap-promote each captured local (allocate
      # a cell, copy current value into it, register so subsequent
      # references in the enclosing scope go through the cell), then
      # allocate the cap struct and populate its pointers.
      k = 0
      while k < captures.length
        vn = captures[k]
        ct = c_type(capture_types[k])
        already_promoted = 0
        ci = 0
        while ci < @heap_promoted_names.length
          if @heap_promoted_names[ci] == vn
            already_promoted = 1
          end
          ci = ci + 1
        end
        if already_promoted == 0
          cell = "_hcell_" + vn + "_p" + pid.to_s
          emit("  " + ct + " *" + cell + " = (" + ct + " *)sp_gc_alloc(sizeof(" + ct + "), NULL, NULL);")
          emit("  *" + cell + " = " + fiber_var_ref(vn) + ";")
          @heap_promoted_names.push(vn)
          @heap_promoted_cells.push(cell)
        end
        k = k + 1
      end
      cap_ptr = "_cap_ptr_p" + pid.to_s
      emit("  " + cap_name + " *" + cap_ptr + " = (" + cap_name + " *)sp_gc_alloc(sizeof(" + cap_name + "), NULL, " + cap_scan_name + ");")
      k = 0
      while k < captures.length
        vn = captures[k]
        ci = 0
        cell = ""
        while ci < @heap_promoted_names.length
          if @heap_promoted_names[ci] == vn
            cell = @heap_promoted_cells[ci]
          end
          ci = ci + 1
        end
        emit("  " + cap_ptr + "->" + vn + " = " + cell + ";")
        k = k + 1
      end
      return "sp_proc_new(" + fname + ", " + cap_ptr + ", " + cap_scan_name + ")"
    end

    # No captures: file-scope function with unused cap arg, sp_proc_new
    # with NULL cap and NULL scan.
    @lambda_funcs << "static mrb_int "
    @lambda_funcs << fname
    @lambda_funcs << "(void *_cap, mrb_int *args) {\n"
    @lambda_funcs << "  (void)_cap;\n"
    @lambda_funcs << proc_fn_args_unpack(bps)
    if body_stmts != ""
      @lambda_funcs << body_stmts
    end
    @lambda_funcs << "  return "
    @lambda_funcs << bexpr
    @lambda_funcs << ";\n}\n"
    return "sp_proc_new(" + fname + ", NULL, NULL)"
  end

  def compile_bracket_assign(nid)
    recv = @nd_receiver[nid]
    rt = infer_type(recv)
    rc = compile_expr_gc_rooted(recv)
    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end
    idx = "0"
    val = "0"
    if arg_ids.length >= 1
      if rt == "str_int_hash" || rt == "str_str_hash"
        idx = compile_expr_as_string(arg_ids[0])
      else
        idx = compile_expr(arg_ids[0])
      end
    end
    if arg_ids.length >= 2
      val = compile_expr(arg_ids[1])
    end
    if rt == "int_str_hash"
      emit("  sp_IntStrHash_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
    if rt == "sym_int_hash"
      emit("  sp_SymIntHash_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
    if rt == "sym_str_hash"
      emit("  sp_SymStrHash_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
    if rt == "sym_poly_hash"
      boxed = val
      if arg_ids.length >= 2
        boxed = box_expr_to_poly(arg_ids[1])
      end
      emit("  sp_SymPolyHash_set(" + rc + ", " + idx + ", " + boxed + ");")
      return
    end
    if rt == "str_poly_hash"
      idx_s = compile_expr_as_string(arg_ids[0])
      boxed = val
      if arg_ids.length >= 2
        boxed = box_expr_to_poly(arg_ids[1])
      end
      emit("  sp_StrPolyHash_set(" + rc + ", " + idx_s + ", " + boxed + ");")
      return
    end
    if rt == "int_array"
      # Check if value is an object pointer - needs cast
      vt = "int"
      if arg_ids.length >= 2
        vt = infer_type(arg_ids[1])
      end
      if is_obj_type(vt) == 1
        emit("  sp_IntArray_set(" + rc + ", " + idx + ", (mrb_int)" + val + ");")
      else
        emit("  sp_IntArray_set(" + rc + ", " + idx + ", " + val + ");")
      end
      return
    end
    if rt == "float_array"
      emit("  sp_FloatArray_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
    if rt == "str_array"
      emit("  sp_StrArray_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
    if rt == "str_int_hash"
      emit("  sp_StrIntHash_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
    if rt == "str_str_hash"
      emit("  sp_StrStrHash_set(" + rc + ", " + idx + ", " + val + ");")
      return
    end
  end

  # Compile `recv[idx] OP= value` (IndexOperatorWriteNode).
  #
  # Emitted as a get-modify-set against the appropriate typed container,
  # in a block scope so that the receiver and index are each evaluated
  # exactly once. Falls through silently for receiver types we don't
  # handle yet — currently float_array, int_array, and the four numeric
  # hash variants. Compound-assign on string arrays / poly hashes / str
  # hashes is rarely useful and would need per-type semantics.
  def compile_index_op_assign(nid)
    recv = @nd_receiver[nid]
    args_id = @nd_arguments[nid]
    arg_ids = args_id >= 0 ? get_args(args_id) : []
    return if arg_ids.length < 1
    op = @nd_binop[nid]
    rt = infer_type(recv)
    rc = compile_expr_gc_rooted(recv)
    idx = compile_expr(arg_ids[0])
    val = compile_expr(@nd_expression[nid])

    if rt == "float_array" || rt == "int_array"
      pfx = array_c_prefix(rt)
      tt  = new_temp
      ti  = new_temp
      emit("  { sp_" + pfx + " *" + tt + " = " + rc + "; mrb_int " + ti + " = " + idx +
           "; sp_" + pfx + "_set(" + tt + ", " + ti +
           ", sp_" + pfx + "_get(" + tt + ", " + ti + ") " + op + " (" + val + ")); }")
      return
    end
    if rt == "str_int_hash"
      tt = new_temp
      ti = new_temp
      idx_s = compile_expr_as_string(arg_ids[0])
      emit("  { sp_StrIntHash *" + tt + " = " + rc + "; const char *" + ti + " = " + idx_s +
           "; sp_StrIntHash_set(" + tt + ", " + ti +
           ", sp_StrIntHash_get(" + tt + ", " + ti + ") " + op + " (" + val + ")); }")
      return
    end
    if rt == "int_str_hash"
      # Concatenating strings (`+= str`) is the only sensible op here.
      if op == "+"
        tt = new_temp
        ti = new_temp
        emit("  { sp_IntStrHash *" + tt + " = " + rc + "; mrb_int " + ti + " = " + idx +
             "; sp_IntStrHash_set(" + tt + ", " + ti +
             ", sp_str_concat(sp_IntStrHash_get(" + tt + ", " + ti + "), " + val + ")); }")
        return
      end
    end
    if rt == "sym_int_hash"
      tt = new_temp
      ti = new_temp
      emit("  { sp_SymIntHash *" + tt + " = " + rc + "; sp_sym " + ti + " = " + idx +
           "; sp_SymIntHash_set(" + tt + ", " + ti +
           ", sp_SymIntHash_get(" + tt + ", " + ti + ") " + op + " (" + val + ")); }")
      return
    end
  end

  # Return a C expression that evaluates to the inspected form of `val`
  # (a value of inferred Ruby type `at`), following Ruby's Object#inspect
  # contract. Returns "" when `at` has no inspect implementation yet, so
  # callers can fall back to their previous behaviour.
  def compile_inspect_for(at, val)
    if at == "int"
      return "sp_int_to_s(" + val + ")"
    end
    if at == "float"
      return "sp_float_inspect(" + val + ")"
    end
    if at == "string" || at == "string?"
      return "sp_str_inspect(" + val + ")"
    end
    if at == "mutable_str"
      return "sp_str_inspect(" + val + "->data)"
    end
    if at == "symbol"
      return "sp_str_concat(\":\", sp_sym_to_s(" + val + "))"
    end
    if at == "bool"
      return "(" + val + " ? \"true\" : \"false\")"
    end
    if at == "nil"
      return "\"nil\""
    end
    if at == "int_array"
      @needs_int_array = 1
      return "sp_IntArray_inspect(" + val + ")"
    end
    if at == "float_array"
      @needs_float_array = 1
      return "sp_FloatArray_inspect(" + val + ")"
    end
    if at == "str_array"
      @needs_str_array = 1
      return "sp_StrArray_inspect(" + val + ")"
    end
    if at == "sym_array"
      @needs_int_array = 1
      return "sp_SymArray_inspect(" + val + ")"
    end
    if at == "poly_array"
      @needs_rb_value = 1
      return "sp_PolyArray_inspect(" + val + ")"
    end
    if at == "poly"
      @needs_rb_value = 1
      return "sp_poly_inspect(" + val + ")"
    end
    ""
  end

  # Kernel#p: for each argument, prints `arg.inspect` followed by a
  # newline. Uses `compile_inspect_for` for types that implement inspect;
  # falls back to puts-style output for types that don't yet (e.g.
  # user-defined classes, ranges, hashes).
  def compile_p(nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      return
    end
    arg_ids = get_args(args_id)
    if arg_ids.length == 0
      return
    end
    k = 0
    while k < arg_ids.length
      aid = arg_ids[k]
      at = infer_type(aid)
      val = compile_expr(aid)
      ins = compile_inspect_for(at, val)
      if ins == ""
        # No inspect implementation for this type — keep the historic
        # behaviour so nothing regresses.
        compile_puts_single(aid, at, val)
      else
        emit("  fputs(" + ins + ", stdout); putchar('" + bsl_n + "');")
      end
      k = k + 1
    end
  end

  # Emit the puts-equivalent for a single arg (extracted for reuse from p).
  def compile_puts_single(aid, at, val)
    if at == "poly"
      @needs_rb_value = 1
      emit("  sp_poly_puts(" + val + ");")
      return
    end
    if at == "mutable_str"
      emit("  { const char *_ps = (const char *)(" + val + "->data); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
      return
    end
    if at == "int"
      emit("  printf(\"%lld" + bsl_n + "\", (long long)" + val + ");")
      return
    end
    if at == "float"
      emit("  { const char *_fs = sp_float_to_s(" + val + "); fputs(_fs, stdout); putchar('" + bsl_n + "'); }")
      return
    end
    if at == "bool"
      emit("  puts(" + val + " ? \"true\" : \"false\");")
      return
    end
    if at == "string" || at == "string?"
      emit("  { const char *_ps = (const char *)(" + val + "); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
      return
    end
    emit("  printf(\"%lld" + bsl_n + "\", (long long)" + val + ");")
  end

  def compile_puts(nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      emit("  putchar('" + bsl_n + "');")
      return
    end
    arg_ids = get_args(args_id)
    if arg_ids.length == 0
      emit("  putchar('" + bsl_n + "');")
      return
    end
    k = 0
    while k < arg_ids.length
      aid = arg_ids[k]
      at = infer_type(aid)
      val = compile_expr(aid)
      if at == "poly"
        @needs_rb_value = 1
        emit("  sp_poly_puts(" + val + ");")
        k = k + 1
        next
      end
      if at == "mutable_str"
        emit("  { const char *_ps = (const char *)(" + val + "->data); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
        k = k + 1
        next
      end
      if at == "bigint"
        emit("  { const char *_bs = sp_bigint_to_s(" + val + "); fputs(_bs, stdout); putchar('" + bsl_n + "'); }")
        k = k + 1
        next
      end
      if at == "symbol"
        emit("  puts(sp_sym_to_s(" + val + "));")
        k = k + 1
        next
      end
      if at == "int"
        emit("  printf(\"%lld" + bsl_n + "\", (long long)" + val + ");")
      else
        if at == "float"
          emit("  { const char *_fs = sp_float_to_s(" + val + "); fputs(_fs, stdout); putchar('" + bsl_n + "'); }")
        else
          if at == "string" || at == "string?"
            emit("  { const char *_ps = (const char *)(" + val + "); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
          else
            if at == "bool"
              emit("  puts(" + val + " ? \"true\" : \"false\");")
            else
              if is_obj_type(at) == 1
                cname = at[4, at.length - 4]
                owner = find_method_owner(find_class_idx(cname), "to_s")
                if owner != ""
                  sv = "sp_" + owner + "_to_s(" + (owner == cname ? val : "(sp_" + owner + " *)" + val) + ")"
                  emit("  { const char *_ps = (const char *)(" + sv + "); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
                else
                  emit("  printf(\"%lld" + bsl_n + "\", (long long)(mrb_int)" + val + ");")
                end
              else
                if at == "str_array"
                  emit("  { sp_StrArray *_pa = " + val + "; for (mrb_int _pi = 0; _pi < _pa->len; _pi++) puts(_pa->data[_pi]); }")
                elsif at == "sym_array"
                  emit("  { sp_IntArray *_pa = " + val + "; for (mrb_int _pi = 0; _pi < _pa->len; _pi++) puts(sp_sym_to_s((sp_sym)_pa->data[_pa->start + _pi])); }")
                elsif at == "int_array"
                  emit("  { sp_IntArray *_pa = " + val + "; for (mrb_int _pi = 0; _pi < _pa->len; _pi++) printf(\"%lld" + bsl_n + "\", (long long)_pa->data[_pa->start + _pi]); }")
                elsif at == "float_array"
                  emit("  { sp_FloatArray *_pa = " + val + "; for (mrb_int _pi = 0; _pi < _pa->len; _pi++) { const char *_fs = sp_float_to_s(_pa->data[_pi]); fputs(_fs, stdout); putchar('" + bsl_n + "'); } }")
                else
                  emit("  printf(\"%lld" + bsl_n + "\", (long long)" + val + ");")
                end
              end
            end
          end
        end
      end
      k = k + 1
    end
  end

  def compile_stderr_puts(nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      emit("  fputc('" + bsl_n + "', stderr);")
      return
    end
    arg_ids = get_args(args_id)
    k = 0
    while k < arg_ids.length
      at = infer_type(arg_ids[k])
      val = compile_expr(arg_ids[k])
      if at == "string"
        emit("  fprintf(stderr, \"%s" + bsl_n + "\", " + val + ");")
      else
        emit("  fprintf(stderr, \"%lld" + bsl_n + "\", (long long)" + val + ");")
      end
      k = k + 1
    end
  end

  def compile_print(nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      return
    end
    arg_ids = get_args(args_id)
    k = 0
    while k < arg_ids.length
      aid = arg_ids[k]
      # Detect x.chr pattern and use putchar for binary-safe output
      if @nd_type[aid] == "CallNode"
        if @nd_name[aid] == "chr"
          if @nd_receiver[aid] >= 0
            rchr = compile_expr(@nd_receiver[aid])
            emit("  putchar((unsigned char)" + rchr + ");")
            k = k + 1
            next
          end
        end
      end
      at = infer_type(aid)
      val = compile_expr(aid)
      if at == "bigint"
        emit("  fputs(sp_bigint_to_s(" + val + "), stdout);")
        k = k + 1
        next
      end
      if at == "symbol"
        emit("  fputs(sp_sym_to_s(" + val + "), stdout);")
        k = k + 1
        next
      end
      if at == "int"
        emit("  printf(\"%lld\", (long long)" + val + ");")
      else
        if at == "mutable_str"
          emit("  fputs(" + val + "->data, stdout);")
        else
          if at == "string"
            emit("  fputs(" + val + ", stdout);")
          else
            emit("  printf(\"%lld\", (long long)" + val + ");")
          end
        end
      end
      k = k + 1
    end
  end

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

  def compile_each_slice_block(nid)
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    n = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_slice"
    end
    tmp_i = new_temp
    tmp_j = new_temp
    tmp_len = new_temp
    pfx = array_c_prefix(rt)
    @needs_gc = 1
    emit("  mrb_int " + tmp_len + " = sp_" + pfx + "_length(" + rc + ");")
    emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < " + tmp_len + "; " + tmp_i + " += " + n + ") {")
    # When bp1 shadows an outer same-named local of a different C type, the
    # function-level lv_<bp1> has the outer type. Emit a block-scoped fresh
    # declaration with its own GC root so the slice survives allocations in
    # the user block body.
    outer_t = find_var_type(bp1)
    if outer_t != "" && outer_t != rt
      emit("    SP_GC_SAVE();")
      emit("    " + c_type(rt) + " lv_" + bp1 + " = sp_" + pfx + "_new();")
      emit("    SP_GC_ROOT(lv_" + bp1 + ");")
    else
      emit("    lv_" + bp1 + " = sp_" + pfx + "_new();")
    end
    emit("    for (mrb_int " + tmp_j + " = 0; " + tmp_j + " < " + n + " && " + tmp_i + " + " + tmp_j + " < " + tmp_len + "; " + tmp_j + "++)")
    emit("      sp_" + pfx + "_push(lv_" + bp1 + ", sp_" + pfx + "_get(" + rc + ", " + tmp_i + " + " + tmp_j + "));")
    @indent = @indent + 1
    push_scope
    declare_var(bp1, rt)
    compile_stmts_body(@nd_body[@nd_block[nid]])
    pop_scope
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_each_cons_block(nid)
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    n = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_cons"
    end
    tmp_i = new_temp
    tmp_j = new_temp
    tmp_len = new_temp
    pfx = array_c_prefix(rt)
    @needs_gc = 1
    emit("  mrb_int " + tmp_len + " = sp_" + pfx + "_length(" + rc + ");")
    emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " + " + n + " <= " + tmp_len + "; " + tmp_i + "++) {")
    # See compile_each_slice_block: shadow case needs a fresh typed slot with
    # its own GC root.
    outer_t = find_var_type(bp1)
    if outer_t != "" && outer_t != rt
      emit("    SP_GC_SAVE();")
      emit("    " + c_type(rt) + " lv_" + bp1 + " = sp_" + pfx + "_new();")
      emit("    SP_GC_ROOT(lv_" + bp1 + ");")
    else
      emit("    lv_" + bp1 + " = sp_" + pfx + "_new();")
    end
    emit("    for (mrb_int " + tmp_j + " = 0; " + tmp_j + " < " + n + "; " + tmp_j + "++)")
    emit("      sp_" + pfx + "_push(lv_" + bp1 + ", sp_" + pfx + "_get(" + rc + ", " + tmp_i + " + " + tmp_j + "));")
    @indent = @indent + 1
    push_scope
    declare_var(bp1, rt)
    compile_stmts_body(@nd_body[@nd_block[nid]])
    pop_scope
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_each_with_object_block(nid)
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    obj_ct = "mrb_int"
    args_id = @nd_arguments[nid]
    if args_id >= 0
      aargs = get_args(args_id)
      if aargs.length > 0
        obj_ct = c_type(infer_type(aargs[0]))
      end
    end
    # Root the seed: it lives across every loop iteration, and any
    # allocation inside the block can trigger a GC that would otherwise
    # sweep the freshly constructed seed object before the loop ends.
    obj_arg_nid = -1
    if args_id >= 0
      aargs2 = get_args(args_id)
      if aargs2.length > 0
        obj_arg_nid = aargs2[0]
      end
    end
    obj_arg = compile_expr_gc_rooted(obj_arg_nid)
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    if bp1 == ""
      bp1 = "_elem"
    end
    if bp2 == ""
      bp2 = "_obj"
    end
    # Outer-scope slot survives the inner `{}` so the expression form
    # of each_with_object can still observe the final accumulator.
    result = new_temp
    emit("  " + obj_ct + " " + result + " = " + obj_arg + ";")
    tmp_i = new_temp
    if is_array_type(rt) == 1
      pfx = array_c_prefix(rt)
      emit("  {")
      @indent = @indent + 1
      emit("  " + obj_ct + " lv_" + bp2 + " = " + result + ";")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_" + pfx + "_length(" + rc + "); " + tmp_i + "++) {")
      emit("    " + c_type(elem_type_of_array(rt)) + " lv_" + bp1 + " = sp_" + pfx + "_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, elem_type_of_array(rt))
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
      emit("  " + result + " = lv_" + bp2 + ";")
      @indent = @indent - 1
      emit("  }")
    end
    @in_loop = old
    result
  end

  def compile_each_with_index_block(nid)
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    if bp1 == ""
      bp1 = "_v"
    end
    if bp2 == ""
      bp2 = "_idx"
    end
    tmp = new_temp
    pfx = array_c_prefix(rt)
    emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_" + pfx + "_length(" + rc + "); " + tmp + "++) {")
    emit("    " + c_type(elem_type_of_array(rt)) + " lv_" + bp1 + " = sp_" + pfx + "_get(" + rc + ", " + tmp + ");")
    emit("    mrb_int lv_" + bp2 + " = " + tmp + ";")
    @indent = @indent + 1
    push_scope
    declare_var(bp1, elem_type_of_array(rt))
    declare_var(bp2, "int")
    compile_stmts_body(@nd_body[@nd_block[nid]])
    pop_scope
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_each_block(nid)
    old = @in_loop
    @in_loop = 1
    # Fuse hash.keys.each → direct order-array loop to avoid intermediate sp_IntArray allocation
    recv_nid = @nd_receiver[nid]
    if recv_nid >= 0 && @nd_type[recv_nid] == "CallNode" && @nd_name[recv_nid] == "keys"
      hash_nid = @nd_receiver[recv_nid]
      if hash_nid >= 0
        ht = infer_type(hash_nid)
        if ht == "int_str_hash" || ht == "str_int_hash" || ht == "str_str_hash" || ht == "sym_int_hash" || ht == "sym_str_hash" || ht == "sym_poly_hash" || ht == "str_poly_hash"
          hrc = compile_expr_gc_rooted(hash_nid)
          bp1 = get_block_param(nid, 0)
          has_bp = 1
          if bp1 == ""
            has_bp = 0
            bp1 = "_x"
          end
          # Pick the key type. Sym-keyed hashes store the sym id in
          # `order[]` as a plain mrb_int — cast to sp_sym so block-body
          # dispatchers see the right type.
          is_sym_key = 0
          if ht == "sym_int_hash" || ht == "sym_str_hash" || ht == "sym_poly_hash"
            is_sym_key = 1
          end
          is_str_key = 0
          if ht == "str_int_hash" || ht == "str_str_hash" || ht == "str_poly_hash"
            is_str_key = 1
          end
          key_type = "int"
          if is_sym_key == 1
            key_type = "symbol"
          elsif is_str_key == 1
            key_type = "string"
          end
          tmp = new_temp
          emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + hrc + "->len; " + tmp + "++) {")
          # Inline-declare `lv_<bp1>` with the key's C type so a block
          # param that shadows an outer same-named local of a different
          # type compiles correctly (mirrors PR #115's pattern).
          if has_bp == 1
            if is_sym_key == 1
              emit("    sp_sym lv_" + bp1 + " = (sp_sym)" + hrc + "->order[" + tmp + "];")
            else
              emit("    " + c_type(key_type) + " lv_" + bp1 + " = " + hrc + "->order[" + tmp + "];")
            end
          end
          @indent = @indent + 1
          push_scope
          if has_bp == 1
            declare_var(bp1, key_type)
          end
          compile_stmts_body(@nd_body[@nd_block[nid]])
          pop_scope
          @indent = @indent - 1
          emit("  }")
          @in_loop = old
          return
        end
      end
    end
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    has_bp = 1
    if bp1 == ""
      has_bp = 0
      bp1 = "_x"
    end

    if rt == "int_array" || rt == "str_array" || rt == "float_array"
      tmp = new_temp
      pfx = array_c_prefix(rt)
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_" + pfx + "_length(" + rc + "); " + tmp + "++) {")
      if has_bp == 1
        emit("    " + c_type(elem_type_of_array(rt)) + " lv_" + bp1 + " = sp_" + pfx + "_get(" + rc + ", " + tmp + ");")
      end
      @indent = @indent + 1
      push_scope
      if has_bp == 1
        declare_var(bp1, elem_type_of_array(rt))
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "sym_array"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
      if has_bp == 1
        emit("    lv_" + bp1 + " = (sp_sym)sp_IntArray_get(" + rc + ", " + tmp + ");")
      end
      @indent = @indent + 1
      push_scope
      if has_bp == 1
        declare_var(bp1, "symbol")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if is_ptr_array_type(rt) == 1
      elem_type = ptr_array_elem_type(rt)
      tmp = new_temp
      bp_tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_PtrArray_length(" + rc + "); " + tmp + "++) {")
      if has_bp == 1
        emit("    " + c_type(elem_type) + " " + bp_tmp + " = (" + c_type(elem_type) + ")sp_PtrArray_get(" + rc + ", " + tmp + ");")
        emit("    lv_" + bp1 + " = " + bp_tmp + ";")
      end
      @indent = @indent + 1
      push_scope
      if has_bp == 1
        declare_var(bp1, elem_type)
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "str_int_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_StrIntHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "string")
      if bp2 != ""
        declare_var(bp2, "int")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "int_str_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_IntStrHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "int")
      if bp2 != ""
        declare_var(bp2, "string")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "str_str_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_StrStrHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "string")
      if bp2 != ""
        declare_var(bp2, "string")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "sym_int_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_SymIntHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "symbol")
      if bp2 != ""
        declare_var(bp2, "int")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "sym_str_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_SymStrHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "symbol")
      if bp2 != ""
        declare_var(bp2, "string")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "sym_poly_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_SymPolyHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "symbol")
      if bp2 != ""
        declare_var(bp2, "poly")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "str_poly_hash"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "->len; " + tmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + tmp + "];")
      if bp2 != ""
        emit("    lv_" + bp2 + " = sp_StrPolyHash_get(" + rc + ", " + rc + "->order[" + tmp + "]);")
      end
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "string")
      if bp2 != ""
        declare_var(bp2, "poly")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "range"
      tmp = new_temp
      tmp2 = new_temp
      emit("  sp_Range " + tmp2 + " = " + rc + ";")
      emit("  for (lv_" + bp1 + " = " + tmp2 + ".first; lv_" + bp1 + " <= " + tmp2 + ".last; lv_" + bp1 + "++) {")
      @indent = @indent + 1
      push_scope
      if has_bp == 1
        declare_var(bp1, "int")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "poly_array"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_PolyArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp1 + " = sp_PolyArray_get(" + rc + ", " + tmp + ");")
      @indent = @indent + 1
      push_scope
      if has_bp == 1
        declare_var(bp1, "poly")
      end
      compile_stmts_body(@nd_body[@nd_block[nid]])
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    @in_loop = old
  end

  def compile_times_block(nid)
    old = @in_loop
    @in_loop = 1
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "; " + tmp + "++) {")
    if bp1 != ""
      emit("    lv_" + bp1 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    push_scope
    if bp1 != ""
      declare_var(bp1, "int")
    end
    compile_stmts_body(@nd_body[@nd_block[nid]])
    pop_scope
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_upto_block(nid)
    old = @in_loop
    @in_loop = 1
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    lim = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = " + rc + "; " + tmp + " <= " + lim + "; " + tmp + "++) {")
    if bp1 != ""
      emit("    lv_" + bp1 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    push_scope
    if bp1 != ""
      declare_var(bp1, "int")
    end
    compile_stmts_body(@nd_body[@nd_block[nid]])
    pop_scope
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_downto_block(nid)
    old = @in_loop
    @in_loop = 1
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    lim = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = " + rc + "; " + tmp + " >= " + lim + "; " + tmp + "--) {")
    if bp1 != ""
      emit("    lv_" + bp1 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    push_scope
    if bp1 != ""
      declare_var(bp1, "int")
    end
    compile_stmts_body(@nd_body[@nd_block[nid]])
    pop_scope
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_tap_expr(nid)
    # Execute block with receiver bound to block param, return receiver.
    # Open a C block so the param is a fresh local that shadows any
    # outer same-named lv_<bp> without clobbering its value or type.
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp = get_block_param(nid, 0)
    if bp == ""
      bp = "_x"
    end
    tmp = new_temp
    emit("  " + c_type(rt) + " " + tmp + " = " + rc + ";")
    emit("  {")
    emit("    " + c_type(rt) + " lv_" + bp + " = " + tmp + ";")
    @indent = @indent + 1
    push_scope
    declare_var(bp, rt)
    blk = @nd_block[nid]
    bbody = @nd_body[blk]
    if bbody >= 0
      bs = get_stmts(bbody)
      k = 0
      while k < bs.length
        compile_stmt(bs[k])
        k = k + 1
      end
    end
    pop_scope
    @indent = @indent - 1
    emit("  }")
    tmp
  end

  def compile_then_expr(nid)
    # Execute block with receiver bound to block param, return block result.
    # Open a C block so the param is a fresh local that shadows any outer
    # same-named lv_<bp>. The block's last-expression value is funneled
    # through a result tmp declared in the enclosing scope so callers can
    # still consume it after the C block closes.
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp = get_block_param(nid, 0)
    if bp == ""
      bp = "_x"
    end
    blk = @nd_block[nid]
    bbody = @nd_body[blk]

    # Peek at the last expression's type under the inner binding so the
    # result tmp is declared with the type that infer_type sees inside
    # the block, not the type of any outer same-named local.
    ret_t = "int"
    bs = []
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        push_scope
        declare_var(bp, rt)
        ret_t = infer_type(bs.last)
        pop_scope
      end
    end

    result_tmp = new_temp
    emit("  " + c_type(ret_t) + " " + result_tmp + " = " + c_default_val(ret_t) + ";")
    emit("  {")
    emit("    " + c_type(rt) + " lv_" + bp + " = " + rc + ";")
    @indent = @indent + 1
    push_scope
    declare_var(bp, rt)
    if bs.length > 0
      k = 0
      while k < bs.length - 1
        compile_stmt(bs[k])
        k = k + 1
      end
      last_expr = compile_expr(bs.last)
      emit("  " + result_tmp + " = " + last_expr + ";")
    end
    pop_scope
    @indent = @indent - 1
    emit("  }")
    result_tmp
  end

  # Emit the loop-open lines for iterating over a receiver expression.
  # Supports range and all array-like types.  After calling this helper,
  # the caller emits the block body and a closing '}'.  idx_var holds
  # the loop counter (position or value for range); elem_var gets the
  # current element.
  def emit_iter_open(rc, recv_type, elem_var, idx_var)
    if recv_type == "range"
      rtmp = new_temp
      emit("  sp_Range " + rtmp + " = " + rc + ";")
      emit("  for (mrb_int " + idx_var + " = " + rtmp + ".first; " + idx_var + " <= " + rtmp + ".last; " + idx_var + "++) {")
      emit("    " + elem_var + " = " + idx_var + ";")
      return
    end
    pfx = array_c_prefix(recv_type)
    emit("  for (mrb_int " + idx_var + " = 0; " + idx_var + " < sp_" + pfx + "_length(" + rc + "); " + idx_var + "++) {")
    emit("    " + elem_var + " = sp_" + pfx + "_get(" + rc + ", " + idx_var + ");")
  end

  # Element type of an iterable (for block param type inference).
  def iter_elem_type(recv_type)
    if recv_type == "range"
      return "int"
    end
    elem_type_of_array(recv_type)
  end

  def compile_array_sum_block(nid, rc, recv_type)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_sum = new_temp
    tmp_i = new_temp
    emit("  mrb_int " + tmp_sum + " = 0;")
    emit_iter_open(rc, recv_type, "lv_" + bp1, tmp_i)
    push_scope
    declare_var(bp1, iter_elem_type(recv_type))
    blk = @nd_block[nid]
    bexpr = "0"
    if @nd_body[blk] >= 0
      bs = get_stmts(@nd_body[blk])
      if bs.length > 0
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        bexpr = compile_expr(bs.last)
      end
    end
    emit("    " + tmp_sum + " += " + bexpr + ";")
    pop_scope
    emit("  }")
    tmp_sum
  end

  def compile_array_count_block(nid, rc, recv_type)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_cnt = new_temp
    tmp_i = new_temp
    emit("  mrb_int " + tmp_cnt + " = 0;")
    emit_iter_open(rc, recv_type, "lv_" + bp1, tmp_i)
    push_scope
    declare_var(bp1, iter_elem_type(recv_type))
    blk = @nd_block[nid]
    bexpr = "0"
    if @nd_body[blk] >= 0
      bs = get_stmts(@nd_body[blk])
      if bs.length > 0
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        bexpr = compile_expr(bs.last)
      end
    end
    emit("    if (" + bexpr + ") " + tmp_cnt + "++;")
    pop_scope
    emit("  }")
    tmp_cnt
  end

  def compile_array_min_max_block(nid, rc, recv_type, mname)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    elem_type = "int"
    if recv_type == "str_array"
      elem_type = "string"
    elsif recv_type == "float_array"
      elem_type = "float"
    end
    tmp_res = new_temp
    tmp_key = new_temp
    tmp_i = new_temp
    bp_tmp = new_temp
    emit("  " + c_type(elem_type) + " " + tmp_res + " = " + c_default_val(elem_type) + ";")
    emit("  mrb_int " + tmp_key + " = 0;")
    emit("  " + c_type(elem_type) + " " + bp_tmp + " = " + c_default_val(elem_type) + ";")
    emit_iter_open(rc, recv_type, "lv_" + bp1, tmp_i)
    emit("    " + bp_tmp + " = lv_" + bp1 + ";")
    push_scope
    declare_var(bp1, elem_type)
    blk = @nd_block[nid]
    bexpr = "0"
    if @nd_body[blk] >= 0
      bs = get_stmts(@nd_body[blk])
      if bs.length > 0
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        bexpr = compile_expr(bs.last)
      end
    end
    cmp = ">"
    if mname == "min"
      cmp = "<"
    end
    emit("    mrb_int _k = " + bexpr + ";")
    emit("    if (" + tmp_i + " == 0 || _k " + cmp + " " + tmp_key + ") { " + tmp_res + " = " + bp_tmp + "; " + tmp_key + " = _k; }")
    pop_scope
    emit("  }")
    tmp_res
  end

  def compile_array_filter_map(nid, rc, recv_type)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    elem_t = iter_elem_type(recv_type)
    push_scope
    declare_var(bp1, elem_t)
    blk = @nd_block[nid]
    block_ret = "int"
    if blk >= 0
      bbody = @nd_body[blk]
      if bbody >= 0
        bbs = get_stmts(bbody)
        if bbs.length > 0
          block_ret = infer_type(bbs.last)
        end
      end
    end
    if block_ret == "string"
      result_type = "str_array"
    elsif block_ret == "float"
      result_type = "float_array"
    else
      result_type = "int_array"
    end
    pfx_dst = array_c_prefix(result_type)
    @needs_gc = 1
    tmp_arr = new_temp
    tmp_i = new_temp
    tmp_val = new_temp
    emit("  " + c_type(result_type) + " " + tmp_arr + " = sp_" + pfx_dst + "_new();")
    emit_iter_open(rc, recv_type, "lv_" + bp1, tmp_i)
    @indent = @indent + 1
    bexpr = "0"
    if blk >= 0
      bbody2 = @nd_body[blk]
      if bbody2 >= 0
        bs = get_stmts(bbody2)
        if bs.length > 0
          k = 0
          while k < bs.length - 1
            compile_stmt(bs[k])
            k = k + 1
          end
          bexpr = compile_expr(bs.last)
        end
      end
    end
    emit("  " + c_type(block_ret) + " " + tmp_val + " = " + bexpr + ";")
    emit("  if (" + tmp_val + ") sp_" + pfx_dst + "_push(" + tmp_arr + ", " + tmp_val + ");")
    @indent = @indent - 1
    pop_scope
    emit("  }")
    tmp_arr
  end

  def compile_array_find_block(nid, rc, recv_type)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    elem_type = iter_elem_type(recv_type)
    tmp_res = new_temp
    tmp_i = new_temp
    emit("  " + c_type(elem_type) + " " + tmp_res + " = " + c_default_val(elem_type) + ";")
    emit_iter_open(rc, recv_type, "lv_" + bp1, tmp_i)
    push_scope
    declare_var(bp1, elem_type)
    blk = @nd_block[nid]
    bbody = @nd_body[blk]
    bexpr = "0"
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        bexpr = compile_expr(bs.last)
      end
    end
    emit("    if (" + bexpr + ") { " + tmp_res + " = lv_" + bp1 + "; break; }")
    pop_scope
    emit("  }")
    tmp_res
  end

  def compile_array_predicate_block(nid, rc, recv_type, mname)
    # Implements any?/all?/none? with block by short-circuit loop
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_res = new_temp
    tmp_i = new_temp
    init_val = "FALSE"
    if mname == "all?" || mname == "none?"
      init_val = "TRUE"
    end
    emit("  mrb_bool " + tmp_res + " = " + init_val + ";")
    emit_iter_open(rc, recv_type, "lv_" + bp1, tmp_i)
    push_scope
    declare_var(bp1, iter_elem_type(recv_type))
    blk = @nd_block[nid]
    bbody = @nd_body[blk]
    bexpr = "0"
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        bexpr = compile_expr(bs.last)
      end
    end
    if mname == "any?"
      emit("    if (" + bexpr + ") { " + tmp_res + " = TRUE; break; }")
    elsif mname == "all?"
      emit("    if (!(" + bexpr + ")) { " + tmp_res + " = FALSE; break; }")
    elsif mname == "one?"
      emit("    if (" + bexpr + ") { if (" + tmp_res + ") { " + tmp_res + " = FALSE; break; } " + tmp_res + " = TRUE; }")
    else
      # none?
      emit("    if (" + bexpr + ") { " + tmp_res + " = FALSE; break; }")
    end
    pop_scope
    emit("  }")
    tmp_res
  end

  def compile_hash_select_reject(nid, hash_type, rc, mname)
    # Build a new hash by filtering entries using the block
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    if bp1 == ""
      bp1 = "_k"
    end
    if bp2 == ""
      bp2 = "_v"
    end
    ctor = ""
    getter = ""
    setter = ""
    val_type = ""
    if hash_type == "str_int_hash"
      ctor = "sp_StrIntHash_new"
      getter = "sp_StrIntHash_get"
      setter = "sp_StrIntHash_set"
      val_type = "int"
      @needs_str_int_hash = 1
    else
      ctor = "sp_StrStrHash_new"
      getter = "sp_StrStrHash_get"
      setter = "sp_StrStrHash_set"
      val_type = "string"
      @needs_str_str_hash = 1
    end
    @needs_gc = 1
    tmp = new_temp
    itmp = new_temp
    emit("  " + c_type(hash_type) + tmp + " = " + ctor + "();")
    emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
    emit("    lv_" + bp1 + " = " + rc + "->order[" + itmp + "];")
    emit("    lv_" + bp2 + " = " + getter + "(" + rc + ", lv_" + bp1 + ");")
    push_scope
    declare_var(bp1, "string")
    declare_var(bp2, val_type)
    blk = @nd_block[nid]
    bbody = @nd_body[blk]
    bexpr = "0"
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        # Emit all but last as stmts
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        bexpr = compile_expr(bs.last)
      end
    end
    cond = bexpr
    if mname == "reject"
      cond = "!(" + bexpr + ")"
    end
    emit("    if (" + cond + ") " + setter + "(" + tmp + ", lv_" + bp1 + ", lv_" + bp2 + ");")
    pop_scope
    emit("  }")
    tmp
  end

  def compile_hash_block_predicate(nid, hash_type, rc, mname)
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    if bp1 == ""
      bp1 = "_k"
    end
    if bp2 == ""
      bp2 = "_v"
    end
    val_type = "int"
    getter = "sp_StrIntHash_get"
    if hash_type == "str_str_hash"
      val_type = "string"
      getter = "sp_StrStrHash_get"
    end
    push_scope
    declare_var(bp1, "string")
    declare_var(bp2, val_type)
    itmp = new_temp
    # Compile block expression
    blk = @nd_block[nid]
    bbody = @nd_body[blk]
    bexpr = "0"
    blk_stmts = "".split(",")
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        k = 0
        while k < bs.length - 1
          blk_stmts.push(bs[k].to_s)
          k = k + 1
        end
        bexpr = "PLACEHOLDER"
      end
    end
    if mname == "count"
      tmp_c = new_temp
      emit("  mrb_int " + tmp_c + " = 0;")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + itmp + "];")
      emit("    lv_" + bp2 + " = " + getter + "(" + rc + ", lv_" + bp1 + ");")
      if bbody >= 0
        bs = get_stmts(bbody)
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        if bs.length > 0
          bexpr = compile_expr(bs.last)
        end
      end
      emit("    if (" + bexpr + ") " + tmp_c + "++;")
      emit("  }")
      pop_scope
      return tmp_c
    end
    if mname == "any?"
      tmp_r = new_temp
      emit("  mrb_bool " + tmp_r + " = FALSE;")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + itmp + "];")
      emit("    lv_" + bp2 + " = " + getter + "(" + rc + ", lv_" + bp1 + ");")
      if bbody >= 0
        bs = get_stmts(bbody)
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        if bs.length > 0
          bexpr = compile_expr(bs.last)
        end
      end
      emit("    if (" + bexpr + ") { " + tmp_r + " = TRUE; break; }")
      emit("  }")
      pop_scope
      return tmp_r
    end
    if mname == "all?"
      tmp_r = new_temp
      emit("  mrb_bool " + tmp_r + " = TRUE;")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + itmp + "];")
      emit("    lv_" + bp2 + " = " + getter + "(" + rc + ", lv_" + bp1 + ");")
      if bbody >= 0
        bs = get_stmts(bbody)
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        if bs.length > 0
          bexpr = compile_expr(bs.last)
        end
      end
      emit("    if (!(" + bexpr + ")) { " + tmp_r + " = FALSE; break; }")
      emit("  }")
      pop_scope
      return tmp_r
    end
    # find / detect — return key of first match
    if mname == "find" || mname == "detect"
      tmp_r = new_temp
      emit("  const char *" + tmp_r + " = \"\";")
      emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < " + rc + "->len; " + itmp + "++) {")
      emit("    lv_" + bp1 + " = " + rc + "->order[" + itmp + "];")
      emit("    lv_" + bp2 + " = " + getter + "(" + rc + ", lv_" + bp1 + ");")
      if bbody >= 0
        bs = get_stmts(bbody)
        k = 0
        while k < bs.length - 1
          compile_stmt(bs[k])
          k = k + 1
        end
        if bs.length > 0
          bexpr = compile_expr(bs.last)
        end
      end
      emit("    if (" + bexpr + ") { " + tmp_r + " = lv_" + bp1 + "; break; }")
      emit("  }")
      pop_scope
      return tmp_r
    end
    pop_scope
    "0"
  end

  def compile_flat_map_expr(nid)
    # flat_map: for each element, block returns an array; concat all results
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    elem_type = "int"
    if rt == "str_array"
      elem_type = "string"
    elsif rt == "float_array"
      elem_type = "float"
    elsif rt == "poly_array"
      elem_type = "poly"
    elsif is_ptr_array_type(rt) == 1
      elem_type = ptr_array_elem_type(rt)
    end
    push_scope
    declare_var(bp1, elem_type)
    blk = @nd_block[nid]
    block_ret = "int_array"
    if blk >= 0
      bbody = @nd_body[blk]
      if bbody >= 0
        bbs = get_stmts(bbody)
        if bbs.length > 0
          block_ret = infer_type(bbs.last)
        end
      end
    end
    # Fall back to receiver type if block doesn't return an array
    if is_array_type(block_ret) == 0
      block_ret = rt
    end
    @needs_gc = 1
    pfx_src = array_c_prefix(rt)
    pfx_dst = array_c_prefix(block_ret)
    tmp_arr = new_temp
    tmp_i = new_temp
    tmp_inner = new_temp
    tmp_j = new_temp
    emit("  " + c_type(block_ret) + tmp_arr + " = sp_" + pfx_dst + "_new();")
    emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_" + pfx_src + "_length(" + rc + "); " + tmp_i + "++) {")
    emit("    lv_" + bp1 + " = sp_" + pfx_src + "_get(" + rc + ", " + tmp_i + ");")
    @indent = @indent + 1
    bbody2 = @nd_body[blk]
    if bbody2 >= 0
      bbs2 = get_stmts(bbody2)
      # Compile all but last statement
      k = 0
      while k < bbs2.length - 1
        compile_stmt(bbs2[k])
        k = k + 1
      end
      last = bbs2.last
      val = compile_expr(last)
      emit("  " + c_type(block_ret) + tmp_inner + " = " + val + ";")
      emit("  for (mrb_int " + tmp_j + " = 0; " + tmp_j + " < sp_" + pfx_dst + "_length(" + tmp_inner + "); " + tmp_j + "++)")
      emit("    sp_" + pfx_dst + "_push(" + tmp_arr + ", sp_" + pfx_dst + "_get(" + tmp_inner + ", " + tmp_j + "));")
    end
    @indent = @indent - 1
    emit("  }")
    pop_scope
    tmp_arr
  end

  def compile_map_expr(nid)
    # N.times.map { |i| ... } -> loop 0..N-1 building an array
    recv_n = @nd_receiver[nid]
    if recv_n >= 0 && @nd_type[recv_n] == "CallNode" && @nd_name[recv_n] == "times" && @nd_block[recv_n] < 0
      @needs_gc = 1
      ncount = compile_expr(@nd_receiver[recv_n])
      bpn = get_block_param(nid, 0)
      res_type = "int"
      blk_n = @nd_block[nid]
      push_scope
      if bpn != ""
        declare_var(bpn, "int")
      end
      if blk_n >= 0
        body_n = @nd_body[blk_n]
        if body_n >= 0
          stmts_n = get_stmts(body_n)
          if stmts_n.length > 0
            res_type = infer_type(stmts_n.last)
          end
        end
      end
      tmp_arrn = new_temp
      tmp_in = new_temp
      if res_type == "string"
        @needs_str_array = 1
        emit("  sp_StrArray *" + tmp_arrn + " = sp_StrArray_new();")
      elsif res_type == "float"
        emit("  sp_FloatArray *" + tmp_arrn + " = sp_FloatArray_new();")
      else
        @needs_int_array = 1
        emit("  sp_IntArray *" + tmp_arrn + " = sp_IntArray_new();")
      end
      emit("  for (mrb_int " + tmp_in + " = 0; " + tmp_in + " < " + ncount + "; " + tmp_in + "++) {")
      if bpn != ""
        emit("    lv_" + bpn + " = " + tmp_in + ";")
      end
      @indent = @indent + 1
      if blk_n >= 0
        body_n2 = @nd_body[blk_n]
        if body_n2 >= 0
          stmts_n2 = get_stmts(body_n2)
          if stmts_n2.length > 0
            k = 0
            while k < stmts_n2.length - 1
              compile_stmt(stmts_n2[k])
              k = k + 1
            end
            lastv = compile_expr(stmts_n2.last)
            if res_type == "string"
              emit("  sp_StrArray_push(" + tmp_arrn + ", " + lastv + ");")
            elsif res_type == "float"
              emit("  sp_FloatArray_push(" + tmp_arrn + ", " + lastv + ");")
            else
              emit("  sp_IntArray_push(" + tmp_arrn + ", " + lastv + ");")
            end
          end
        end
      end
      @indent = @indent - 1
      emit("  }")
      pop_scope
      return tmp_arrn
    end
    rt = infer_type(@nd_receiver[nid])
    rc_expr = compile_expr(@nd_receiver[nid])
    # Store receiver in a temp to avoid re-evaluation
    rc_tmp = new_temp
    emit("  " + c_type(rt) + " " + rc_tmp + " = " + rc_expr + ";")
    rc = rc_tmp
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array" || rt == "sym_array"
      @needs_int_array = 1
      @needs_gc = 1
      bp_t = elem_type_of_array(rt)
      # Check if block param is used as lambda (elements are lambda pointers in IntArray)
      blk = @nd_block[nid]
      bp_is_lambda = 0
      if blk >= 0
        bp_is_lambda = param_used_as_lambda(bp1, @nd_body[blk])
      end
      # Also check if bp is passed to a function expecting lambda
      if bp_is_lambda == 0
        if blk >= 0
          body2 = @nd_body[blk]
          if body2 >= 0
            stmts2 = get_stmts(body2)
            k2 = 0
            while k2 < stmts2.length
              if @nd_type[stmts2[k2]] == "CallNode"
                cn2 = @nd_name[stmts2[k2]]
                fmi2 = find_method_idx(cn2)
                if fmi2 >= 0
                  fpt2 = @meth_param_types[fmi2].split(",")
                  if fpt2.length > 0
                    if fpt2[0] == "lambda"
                      bp_is_lambda = 1
                    end
                  end
                end
              end
              k2 = k2 + 1
            end
          end
        end
      end
      if bp_is_lambda == 1
        bp_t = "lambda"
      end
      push_scope
      declare_var(bp1, bp_t)
      block_ret = "int"
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            block_ret = infer_type(stmts.last)
          end
        end
      end
      if block_ret == "string"
        @needs_str_array = 1
        emit("  sp_StrArray *" + tmp_arr + " = sp_StrArray_new();")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
        if bp_is_lambda == 1
          emit("    sp_Val * lv_" + bp1 + " = (sp_Val *)sp_IntArray_get(" + rc + ", " + tmp_i + ");")
        else
          emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
        end
        @indent = @indent + 1
        blk2 = @nd_block[nid]
        if blk2 >= 0
          body3 = @nd_body[blk2]
          if body3 >= 0
            stmts3 = get_stmts(body3)
            if stmts3.length > 0
              last = stmts3[stmts3.length - 1]
              val = compile_expr(last)
              emit("  sp_StrArray_push(" + tmp_arr + ", " + val + ");")
            end
          end
        end
        @indent = @indent - 1
        emit("  }")
        pop_scope
        return tmp_arr
      else
        emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
        if bp_is_lambda == 1
          emit("    sp_Val * lv_" + bp1 + " = (sp_Val *)sp_IntArray_get(" + rc + ", " + tmp_i + ");")
        else
          emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
        end
        @indent = @indent + 1
        blk2 = @nd_block[nid]
        if blk2 >= 0
          body3 = @nd_body[blk2]
          if body3 >= 0
            stmts3 = get_stmts(body3)
            if stmts3.length > 0
              last = stmts3[stmts3.length - 1]
              val = compile_expr(last)
              emit("  sp_IntArray_push(" + tmp_arr + ", " + val + ");")
            end
          end
        end
        @indent = @indent - 1
        emit("  }")
        pop_scope
        return tmp_arr
      end
    end
    if rt == "str_array"
      # str_array.map { |s| ... } produced no result branch before, so
      # `tt = foo.map { ... }` silently became `lv_tt = 0` and the
      # subsequent iteration crashed. Issue #43.
      @needs_gc = 1
      push_scope
      declare_var(bp1, "string")
      block_ret = "string"
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            block_ret = infer_type(stmts.last)
          end
        end
      end
      if block_ret == "int"
        @needs_int_array = 1
        emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_StrArray_length(" + rc + "); " + tmp_i + "++) {")
        # Declare lv_<bp> with the block-local type inside the for body
        # so it C-shadows any outer same-named local (Ruby block-local
        # parameter scope: i in `foo.map { |i| ... }` is independent of
        # any outer i).
        emit("    const char *lv_" + bp1 + " = sp_StrArray_get(" + rc + ", " + tmp_i + ");")
        @indent = @indent + 1
        if blk >= 0
          body3 = @nd_body[blk]
          if body3 >= 0
            stmts3 = get_stmts(body3)
            if stmts3.length > 0
              k = 0
              while k < stmts3.length - 1
                compile_stmt(stmts3[k])
                k = k + 1
              end
              val = compile_expr(stmts3.last)
              emit("  sp_IntArray_push(" + tmp_arr + ", " + val + ");")
            end
          end
        end
        @indent = @indent - 1
        emit("  }")
        pop_scope
        return tmp_arr
      end
      @needs_str_array = 1
      emit("  sp_StrArray *" + tmp_arr + " = sp_StrArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_StrArray_length(" + rc + "); " + tmp_i + "++) {")
      # Block-local C decl, see note in the int-block branch above.
      emit("    const char *lv_" + bp1 + " = sp_StrArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      if blk >= 0
        body3 = @nd_body[blk]
        if body3 >= 0
          stmts3 = get_stmts(body3)
          if stmts3.length > 0
            k = 0
            while k < stmts3.length - 1
              compile_stmt(stmts3[k])
              k = k + 1
            end
            val = compile_expr(stmts3.last)
            emit("  sp_StrArray_push(" + tmp_arr + ", " + val + ");")
          end
        end
      end
      @indent = @indent - 1
      emit("  }")
      pop_scope
      return tmp_arr
    end
    "0"
  end

  def compile_select_expr(nid)
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array" || rt == "sym_array"
      @needs_int_array = 1
      @needs_gc = 1
      bp_t = elem_type_of_array(rt)
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, bp_t)
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            last = stmts.last
            cond = compile_expr(last)
            emit("  if (" + cond + ") sp_IntArray_push(" + tmp_arr + ", lv_" + bp1 + ");")
          end
        end
      end
      pop_scope
      @indent = @indent - 1
      emit("  }")
      return tmp_arr
    end
    "0"
  end

  def compile_reduce_expr(nid)
    # Emit the reduce loop as side effects, return accumulator
    compile_reduce_block(nid)
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_acc"
    end
    "lv_" + bp1
  end

  def compile_reduce_block(nid)
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    init_val = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    if bp1 == ""
      bp1 = "_acc"
    end
    if bp2 == ""
      bp2 = "_x"
    end
    emit("  lv_" + bp1 + " = " + init_val + ";")
    rt = infer_type(@nd_receiver[nid])
    pfx = array_c_prefix(rt)
    elem_t = elem_type_of_array(rt)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_" + pfx + "_length(" + rc + "); " + tmp + "++) {")
    emit("    " + c_type(elem_t) + " lv_" + bp2 + " = sp_" + pfx + "_get(" + rc + ", " + tmp + ");")
    @indent = @indent + 1
    push_scope
    declare_var(bp1, elem_t)
    declare_var(bp2, elem_t)
    blk = @nd_block[nid]
    if blk >= 0
      body = @nd_body[blk]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          last = stmts.last
          val = compile_expr(last)
          emit("  lv_" + bp1 + " = " + val + ";")
        end
      end
    end
    pop_scope
    @indent = @indent - 1
    emit("  }")
  end

  def compile_reject_expr(nid)
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    rt = infer_type(@nd_receiver[nid])
    if rt == "int_array" || rt == "sym_array"
      @needs_int_array = 1
      bp_t = elem_type_of_array(rt)
      tmp_arr = new_temp
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      tmp_i = new_temp
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, bp_t)
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            last = stmts.last
            cond = compile_expr(last)
            emit("  if (!(" + cond + ")) sp_IntArray_push(" + tmp_arr + ", lv_" + bp1 + ");")
          end
        end
      end
      pop_scope
      @indent = @indent - 1
      emit("  }")
      return tmp_arr
    end
    "0"
  end

  def compile_reject_block(nid)
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    rt = infer_type(@nd_receiver[nid])
    if rt == "int_array"
      @needs_int_array = 1
      tmp_arr = new_temp
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      tmp_i = new_temp
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "int")
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            last = stmts.last
            cond = compile_expr(last)
            emit("  if (!(" + cond + ")) sp_IntArray_push(" + tmp_arr + ", lv_" + bp1 + ");")
          end
        end
      end
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
  end

  def compile_sprintf_call(nid)
    args_id = @nd_arguments[nid]
    if args_id < 0
      return "\"\""
    end
    arg_ids = get_args(args_id)
    if arg_ids.length == 0
      return "\"\""
    end
    # First arg is format string, rest are values
    fmt = compile_expr(arg_ids[0])
    result = "sp_sprintf(" + fmt
    k = 1
    while k < arg_ids.length
      at = infer_type(arg_ids[k])
      if at == "float"
        result = result + ", " + compile_expr(arg_ids[k])
      else
        result = result + ", " + compile_expr(arg_ids[k])
      end
      k = k + 1
    end
    result + ")"
  end

  def compile_catch_expr(nid)
    @needs_setjmp = 1
    tag = compile_str_arg0(nid)
    blk = @nd_block[nid]
    tmp = new_temp

    emit("  mrb_int " + tmp + " = 0;")
    emit("  sp_catch_tag[sp_catch_top] = " + tag + ";")
    emit("  sp_catch_top++;")
    emit("  if (setjmp(sp_catch_stack[sp_catch_top-1]) == 0) {")
    @indent = @indent + 1
    if blk >= 0
      body = @nd_body[blk]
      if body >= 0
        stmts = get_stmts(body)
        # Compile all but last as statements
        k = 0
        while k < stmts.length - 1
          compile_stmt(stmts[k])
          k = k + 1
        end
        if stmts.length > 0
          last = stmts.last
          emit("  " + tmp + " = " + compile_expr(last) + ";")
        end
      end
    end
    @indent = @indent - 1
    emit("    sp_catch_top--;")
    emit("  } else {")
    emit("    sp_catch_top--;")
    emit("    " + tmp + " = sp_catch_val[sp_catch_top];")
    emit("  }")
    tmp
  end

  def compile_catch_stmt(nid)
    @needs_setjmp = 1
    # catch(:tag) do ... end
    tag = compile_str_arg0(nid)
    blk = @nd_block[nid]
    emit("  sp_catch_tag[sp_catch_top] = " + tag + ";")
    emit("  sp_catch_top++;")
    emit("  if (setjmp(sp_catch_stack[sp_catch_top-1]) == 0) {")
    @indent = @indent + 1
    if blk >= 0
      compile_stmts_body(@nd_body[blk])
    end
    @indent = @indent - 1
    emit("    sp_catch_top--;")
    emit("  } else {")
    emit("    sp_catch_top--;")
    emit("  }")
  end

  def compile_throw_stmt(nid)
    @needs_setjmp = 1
    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end
    tag = "\"\""
    val = "0"
    if arg_ids.length >= 1
      tag = compile_expr_as_string(arg_ids[0])
    end
    if arg_ids.length >= 2
      val = compile_expr(arg_ids[1])
    end
    emit("  sp_throw(" + tag + ", " + val + ");")
  end

  def compile_begin_stmt(nid)
    @needs_setjmp = 1
    has_rescue = @nd_rescue_clause[nid] >= 0
    has_ensure = @nd_ensure_clause[nid] >= 0

    # Check if rescue body has retry
    has_retry = 0
    rc = @nd_rescue_clause[nid]
    if rc >= 0
      if body_has_retry(@nd_body[rc]) == 1
        has_retry = 1
      end
    end

    if has_retry == 1
      emit("  for (;;) {")
      @indent = @indent + 1
    end

    if has_rescue
      emit("  sp_exc_top++;")
      emit("  if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[nid])
      @indent = @indent - 1
      emit("    sp_exc_top--;")
      if has_retry == 1
        emit("    break;")
      end

      rc = @nd_rescue_clause[nid]
      if rc >= 0
        emit("  } else {")
        emit("    sp_exc_top--;")
        @indent = @indent + 1
        # Check for multiple rescue clauses with exception types
        compile_rescue_chain(rc, has_retry)
        @indent = @indent - 1
      end
      emit("  }")
    else
      compile_stmts_body(@nd_body[nid])
    end

    if has_retry == 1
      @indent = @indent - 1
      emit("  }")
    end

    if has_ensure
      ec = @nd_ensure_clause[nid]
      if ec >= 0
        compile_stmts_body(@nd_body[ec])
      end
    end
  end

  def compile_rescue_chain(rc, has_retry)
    # Check for exception type matching
    exc_types = parse_id_list(@nd_exceptions[rc])
    ref = @nd_reference[rc]
    has_type_check = 0
    if exc_types.length > 0
      has_type_check = 1
      # Build condition: sp_exc_is_a(sp_last_exc_cls, "ClassName")
      cond = ""
      k = 0
      while k < exc_types.length
        if k > 0
          cond = cond + " || "
        end
        cond = cond + "sp_exc_is_a((const char*)sp_last_exc_cls, \"" + @nd_name[exc_types[k]] + "\")"
        k = k + 1
      end
      emit("  if (" + cond + ") {")
      @indent = @indent + 1
    end
    if ref >= 0
      rname = @nd_name[ref]
      emit("  lv_" + rname + " = sp_exc_msg[sp_exc_top];")
    end
    compile_rescue_body(@nd_body[rc], has_retry)
    if has_type_check == 1
      @indent = @indent - 1
      # Check for subsequent rescue
      sub = @nd_subsequent[rc]
      if sub >= 0
        emit("  } else {")
        @indent = @indent + 1
        compile_rescue_chain(sub, has_retry)
        @indent = @indent - 1
      end
      emit("  }")
    end
    # Bare rescue catches all, so any subsequent clause is unreachable
    # and we deliberately don't recurse.
    0
  end

  def compile_rescue_body(nid, has_retry)
    if nid < 0
      return
    end
    stmts = get_stmts(nid)
    k = 0
    while k < stmts.length
      compile_stmt(stmts[k])
      k = k + 1
    end
    if has_retry == 1
      # If we get here without a retry (continue), break out of the loop
      emit("  break;")
    end
  end

  def body_has_retry(nid)
    if nid < 0
      return 0
    end
    if @nd_type[nid] == "RetryNode"
      return 1
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      if body_has_retry(stmts[k]) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_body[nid] >= 0
      if body_has_retry(@nd_body[nid]) == 1
        return 1
      end
    end
    if @nd_subsequent[nid] >= 0
      if body_has_retry(@nd_subsequent[nid]) == 1
        return 1
      end
    end
    0
  end

  def compile_yield_stmt(nid)
    args_id = @nd_arguments[nid]
    arg_exprs = ""
    if args_id >= 0
      aids = get_args(args_id)
      k = 0
      while k < aids.length
        if k > 0
          arg_exprs = arg_exprs + ", "
        end
        arg_exprs = arg_exprs + compile_expr(aids[k])
        k = k + 1
      end
    end
    if arg_exprs != ""
      emit("  if (_block) _block(" + arg_exprs + ", _benv);")
    else
      emit("  if (_block) _block(0, _benv);")
    end
  end

  def compile_yield_call_stmt(nid, mi)
    # Call a yield-using top-level function with a block
    # Inline the function body, replacing yield with block body
    blk = @nd_block[nid]
    if blk < 0
      return
    end

    # Get block params
    bp_names = "".split(",")
    bp = @nd_parameters[blk]
    if bp >= 0
      inner = @nd_parameters[bp]
      if inner >= 0
        reqs = parse_id_list(@nd_requireds[inner])
        k = 0
        while k < reqs.length
          bp_names.push(@nd_name[reqs[k]])
          k = k + 1
        end
      end
    end

    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end

    # Declare and set the function's params as new temp vars
    pnames = @meth_param_names[mi].split(",")
    ptypes = @meth_param_types[mi].split(",")
    # Create unique temp names for function params to avoid collision
    @block_counter = @block_counter + 1
    suffix = "_y" + @block_counter.to_s
    param_map_from = "".split(",")
    param_map_to = "".split(",")
    k = 0
    while k < pnames.length
      pt = "int"
      if k < ptypes.length
        pt = ptypes[k]
      end
      tname = pnames[k] + suffix
      val = "0"
      if k < arg_ids.length
        val = compile_expr(arg_ids[k])
      end
      emit("  " + c_type(pt) + " lv_" + tname + " = " + val + ";")
      param_map_from.push(pnames[k])
      param_map_to.push(tname)
      declare_var(tname, pt)
      k = k + 1
    end

    # Also need to declare the function's local vars
    bid = @meth_body_ids[mi]
    if bid >= 0
      flocals_n = "".split(",")
      flocals_t = "".split(",")
      scan_locals(bid, flocals_n, flocals_t, pnames)
      k = 0
      while k < flocals_n.length
        tname = flocals_n[k] + suffix
        emit("  " + c_type(flocals_t[k]) + " lv_" + tname + " = " + c_default_val(flocals_t[k]) + ";")
        param_map_from.push(flocals_n[k])
        param_map_to.push(tname)
        declare_var(tname, flocals_t[k])
        k = k + 1
      end
    end

    # Compile function body, replacing yield with block body
    # and renaming function locals to temp names
    if bid >= 0
      stmts = get_stmts(bid)
      k = 0
      while k < stmts.length
        compile_stmt_with_block(stmts[k], blk, bp_names, param_map_from, param_map_to)
        k = k + 1
      end
    end
  end

  def compile_stmt_with_block(nid, blk, bp_names, map_from, map_to)
    if nid < 0
      return
    end
    t = @nd_type[nid]
    if t == "YieldNode"
      # Replace yield with the block body
      args_id = @nd_arguments[nid]
      if args_id >= 0
        aids = get_args(args_id)
        k = 0
        while k < aids.length
          if k < bp_names.length
            emit("  lv_" + bp_names[k] + " = " + compile_expr_remap(aids[k], map_from, map_to) + ";")
          end
          k = k + 1
        end
      end
      body = @nd_body[blk]
      if body >= 0
        stmts = get_stmts(body)
        sk = 0
        while sk < stmts.length
          compile_stmt(stmts[sk])
          sk = sk + 1
        end
      end
      return
    end
    if t == "LocalVariableWriteNode"
      lname = @nd_name[nid]
      rname = remap_local(lname, map_from, map_to)
      val = compile_expr_remap(@nd_expression[nid], map_from, map_to)
      emit("  lv_" + rname + " = " + val + ";")
      return
    end
    if t == "LocalVariableOperatorWriteNode"
      lname = @nd_name[nid]
      rname = remap_local(lname, map_from, map_to)
      op = @nd_binop[nid]
      val = compile_expr_remap(@nd_expression[nid], map_from, map_to)
      if op == "+"
        emit("  lv_" + rname + " += " + val + ";")
      end
      if op == "-"
        emit("  lv_" + rname + " -= " + val + ";")
      end
      if op == "*"
        emit("  lv_" + rname + " *= " + val + ";")
      end
      return
    end
    if t == "WhileNode"
      old = @in_loop
      @in_loop = 1
      cond = compile_expr_remap(@nd_predicate[nid], map_from, map_to)
      emit("  while (" + cond + ") {")
      @indent = @indent + 1
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        sk = 0
        while sk < stmts.length
          compile_stmt_with_block(stmts[sk], blk, bp_names, map_from, map_to)
          sk = sk + 1
        end
      end
      @indent = @indent - 1
      emit("  }")
      @in_loop = old
      return
    end
    if t == "IfNode"
      cond = compile_expr_remap(@nd_predicate[nid], map_from, map_to)
      emit("  if (" + cond + ") {")
      @indent = @indent + 1
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        sk = 0
        while sk < stmts.length
          compile_stmt_with_block(stmts[sk], blk, bp_names, map_from, map_to)
          sk = sk + 1
        end
      end
      @indent = @indent - 1
      sub = @nd_subsequent[nid]
      if sub >= 0
        if @nd_type[sub] == "ElseNode"
          emit("  } else {")
          @indent = @indent + 1
          eb = @nd_body[sub]
          if eb >= 0
            estmts = get_stmts(eb)
            sk = 0
            while sk < estmts.length
              compile_stmt_with_block(estmts[sk], blk, bp_names, map_from, map_to)
              sk = sk + 1
            end
          end
          @indent = @indent - 1
        else
          emit("  } else")
          compile_stmt_with_block(sub, blk, bp_names, map_from, map_to)
          return
        end
      end
      emit("  }")
      return
    end
    if t == "CallNode"
      # Check if block_given? with remap
      if @nd_name[nid] == "block_given?"
        if @nd_receiver[nid] < 0
          # In inlined context, block IS given, do nothing
          return
        end
      end
      # Handle nested each/times with yield inside block
      if @nd_block[nid] >= 0
        mname2 = @nd_name[nid]
        if mname2 == "each"
          # Compile the each loop, but inside the block body, recurse with yield replacement
          compile_each_with_yield_inline(nid, blk, bp_names, map_from, map_to)
          return
        end
      end
      # Compile call with remapping for args
      val = compile_expr_remap(nid, map_from, map_to)
      if val != "0"
        emit("  " + val + ";")
      end
      return
    end
    if t == "ReturnNode"
      # In inlined context, return becomes a value assignment
      # but for simplicity, just skip
      return
    end
    # Default: for expression statements, use remap
    val = compile_expr_remap(nid, map_from, map_to)
    if val != "0"
      emit("  " + val + ";")
    end
  end

  def remap_local(name, map_from, map_to)
    k = 0
    while k < map_from.length
      if map_from[k] == name
        return map_to[k]
      end
      k = k + 1
    end
    name
  end

  def compile_expr_remap(nid, map_from, map_to)
    if nid < 0
      return "0"
    end
    t = @nd_type[nid]
    if t == "LocalVariableReadNode"
      rname = remap_local(@nd_name[nid], map_from, map_to)
      return "lv_" + rname
    end
    if t == "InstanceVariableReadNode"
      # Remap self to the _yself variable
      self_name = remap_local("_self_", map_from, map_to)
      return self_name + "->" + sanitize_ivar(@nd_name[nid])
    end
    if t == "SelfNode"
      return remap_local("_self_", map_from, map_to)
    end
    if t == "CallNode"
      if @nd_name[nid] == "block_given?"
        if @nd_receiver[nid] < 0
          return "1"
        end
      end
      # For operators with remapped locals
      mname = @nd_name[nid]
      recv = @nd_receiver[nid]
      if recv >= 0
        if mname == "+"
          return "(" + compile_expr_remap(recv, map_from, map_to) + " + " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "-"
          args_id = @nd_arguments[nid]
          if args_id < 0
            return "(-" + compile_expr_remap(recv, map_from, map_to) + ")"
          end
          return "(" + compile_expr_remap(recv, map_from, map_to) + " - " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "*"
          return "(" + compile_expr_remap(recv, map_from, map_to) + " * " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "/"
          return "(" + compile_expr_remap(recv, map_from, map_to) + " / " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "%"
          return "sp_imod(" + compile_expr_remap(recv, map_from, map_to) + ", " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "<"
          return "(" + compile_expr_remap(recv, map_from, map_to) + " < " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == ">"
          return "(" + compile_expr_remap(recv, map_from, map_to) + " > " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "<="
          return "(" + compile_expr_remap(recv, map_from, map_to) + " <= " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == ">="
          return "(" + compile_expr_remap(recv, map_from, map_to) + " >= " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "=="
          return "(" + compile_expr_remap(recv, map_from, map_to) + " == " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
        if mname == "!="
          return "(" + compile_expr_remap(recv, map_from, map_to) + " != " + compile_expr_remap_arg0(nid, map_from, map_to) + ")"
        end
      end
    end
    # Fallback: compile normally
    compile_expr(nid)
  end

  def compile_expr_remap_arg0(nid, map_from, map_to)
    args_id = @nd_arguments[nid]
    if args_id >= 0
      arg_ids = get_args(args_id)
      if arg_ids.length > 0
        return compile_expr_remap(arg_ids[0], map_from, map_to)
      end
    end
    "0"
  end

  def compile_yield_method_call_stmt(nid, cci, midx, mname)
    # Call a yield-using class method with a block - inline the method body
    blk = @nd_block[nid]
    if blk < 0
      return
    end
    bp_names = "".split(",")
    bp = @nd_parameters[blk]
    if bp >= 0
      inner = @nd_parameters[bp]
      if inner >= 0
        reqs = parse_id_list(@nd_requireds[inner])
        k = 0
        while k < reqs.length
          bp_names.push(@nd_name[reqs[k]])
          k = k + 1
        end
      end
    end

    recv = @nd_receiver[nid]
    rc = compile_expr_gc_rooted(recv)

    bodies = @cls_meth_bodies[cci].split(";")
    bid = -1
    if midx < bodies.length
      bid = bodies[midx].to_i
    end

    saved_ci = @current_class_idx
    @current_class_idx = cci

    @block_counter = @block_counter + 1
    suffix = "_y" + @block_counter.to_s

    # For the method params and locals, create remapped names
    all_params = @cls_meth_params[cci].split("|")
    all_ptypes = @cls_meth_ptypes[cci].split("|")
    pnames = "".split(",")
    ptypes = "".split(",")
    if midx < all_params.length
      pnames = all_params[midx].split(",")
    end
    if midx < all_ptypes.length
      ptypes = all_ptypes[midx].split(",")
    end

    map_from = "".split(",")
    map_to = "".split(",")

    # Map self to the receiver expression
    map_from.push("_self_")
    map_to.push(rc)

    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end

    k = 0
    while k < pnames.length
      pt = "int"
      if k < ptypes.length
        pt = ptypes[k]
      end
      tname = pnames[k] + suffix
      val = "0"
      if k < arg_ids.length
        val = compile_expr(arg_ids[k])
      end
      emit("  " + c_type(pt) + " lv_" + tname + " = " + val + ";")
      map_from.push(pnames[k])
      map_to.push(tname)
      declare_var(tname, pt)
      k = k + 1
    end

    # Declare function locals
    if bid >= 0
      flocals_n = "".split(",")
      flocals_t = "".split(",")
      scan_locals(bid, flocals_n, flocals_t, pnames)
      k = 0
      while k < flocals_n.length
        tname = flocals_n[k] + suffix
        emit("  " + c_type(flocals_t[k]) + " lv_" + tname + " = " + c_default_val(flocals_t[k]) + ";")
        map_from.push(flocals_n[k])
        map_to.push(tname)
        declare_var(tname, flocals_t[k])
        k = k + 1
      end
    end

    # Compile the method body inline with yield -> block body
    if bid >= 0
      stmts = get_stmts(bid)
      k = 0
      while k < stmts.length
        compile_stmt_with_block(stmts[k], blk, bp_names, map_from, map_to)
        k = k + 1
      end
    end

    @current_class_idx = saved_ci
  end

  def compile_each_with_yield_inline(nid, outer_blk, outer_bp_names, map_from, map_to)
    # An each call on an array inside an inlined yield function
    # The each block contains yield statements that should be replaced with outer block body
    recv = @nd_receiver[nid]
    recv_expr = compile_expr_remap(recv, map_from, map_to)
    rt = infer_type(recv)
    # If recv is remapped, get the actual type
    if @nd_type[recv] == "InstanceVariableReadNode"
      if @current_class_idx >= 0
        rt = cls_ivar_type(@current_class_idx, @nd_name[recv])
      end
    end

    inner_bp1 = get_block_param(nid, 0)
    if inner_bp1 == ""
      inner_bp1 = "_ex"
    end
    # The inner block param might collide with outer names, so remap it
    inner_bp_remapped = remap_local(inner_bp1, map_from, map_to)

    old = @in_loop
    @in_loop = 1
    tmp = new_temp

    if rt == "int_array"
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + recv_expr + "); " + tmp + "++) {")
      emit("    lv_" + inner_bp_remapped + " = sp_IntArray_get(" + recv_expr + ", " + tmp + ");")
      @indent = @indent + 1
      # Compile inner block body, replacing yield with outer block body
      inner_blk = @nd_block[nid]
      if inner_blk >= 0
        ibody = @nd_body[inner_blk]
        if ibody >= 0
          istmts = get_stmts(ibody)
          sk = 0
          while sk < istmts.length
            # In the inner block, yield should be replaced with outer block body
            inner_nid = istmts[sk]
            if @nd_type[inner_nid] == "YieldNode"
              # yield x -> set outer bp from inner bp, then run outer block body
              yargs = @nd_arguments[inner_nid]
              if yargs >= 0
                yaids = get_args(yargs)
                yk = 0
                while yk < yaids.length
                  if yk < outer_bp_names.length
                    emit("  lv_" + outer_bp_names[yk] + " = " + compile_expr_remap(yaids[yk], map_from, map_to) + ";")
                  end
                  yk = yk + 1
                end
              end
              obody = @nd_body[outer_blk]
              if obody >= 0
                ostmts = get_stmts(obody)
                ok = 0
                while ok < ostmts.length
                  compile_stmt(ostmts[ok])
                  ok = ok + 1
                end
              end
            else
              compile_stmt_with_block(inner_nid, outer_blk, outer_bp_names, map_from, map_to)
            end
            sk = sk + 1
          end
        end
      end
      @indent = @indent - 1
      emit("  }")
    end
    @in_loop = old
  end

  def compile_map_block(nid)
    @needs_int_array = 1
    @needs_gc = 1
    old = @in_loop
    @in_loop = 1
    # N.times.map { |i| ... } -> build int_array with block body; param = index
    recv = @nd_receiver[nid]
    times_recv = 0
    if recv >= 0
      if @nd_type[recv] == "CallNode"
        if @nd_name[recv] == "times"
          if @nd_block[recv] < 0
            times_recv = 1
          end
        end
      end
    end
    if times_recv == 1
      ncount = compile_expr(@nd_receiver[recv])
      bpn = get_block_param(nid, 0)
      tmp_arrn = new_temp
      tmp_in = new_temp
      push_scope
      if bpn != ""
        declare_var(bpn, "int")
      end
      res_type = "int"
      blk_n = @nd_block[nid]
      if blk_n >= 0
        body_n = @nd_body[blk_n]
        if body_n >= 0
          stmts_n = get_stmts(body_n)
          if stmts_n.length > 0
            res_type = infer_type(stmts_n.last)
          end
        end
      end
      if res_type == "string"
        @needs_str_array = 1
        emit("  sp_StrArray *" + tmp_arrn + " = sp_StrArray_new();")
      elsif res_type == "float"
        emit("  sp_FloatArray *" + tmp_arrn + " = sp_FloatArray_new();")
      else
        emit("  sp_IntArray *" + tmp_arrn + " = sp_IntArray_new();")
      end
      emit("  for (mrb_int " + tmp_in + " = 0; " + tmp_in + " < " + ncount + "; " + tmp_in + "++) {")
      if bpn != ""
        emit("    lv_" + bpn + " = " + tmp_in + ";")
      end
      @indent = @indent + 1
      if blk_n >= 0
        body_n2 = @nd_body[blk_n]
        if body_n2 >= 0
          stmts_n2 = get_stmts(body_n2)
          if stmts_n2.length > 0
            k = 0
            while k < stmts_n2.length - 1
              compile_stmt(stmts_n2[k])
              k = k + 1
            end
            lastv = compile_expr(stmts_n2.last)
            if res_type == "string"
              emit("  sp_StrArray_push(" + tmp_arrn + ", " + lastv + ");")
            elsif res_type == "float"
              emit("  sp_FloatArray_push(" + tmp_arrn + ", " + lastv + ");")
            else
              emit("  sp_IntArray_push(" + tmp_arrn + ", " + lastv + ");")
            end
          end
        end
      end
      @indent = @indent - 1
      emit("  }")
      pop_scope
      @in_loop = old
      return
    end
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array"
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "int")
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            last = stmts.last
            val = compile_expr(last)
            emit("  sp_IntArray_push(" + tmp_arr + ", " + val + ");")
          end
        end
      end
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "str_array"
      @needs_str_array = 1
      emit("  sp_StrArray *" + tmp_arr + " = sp_StrArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_StrArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    const char *lv_" + bp1 + " = sp_StrArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, "string")
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            last = stmts.last
            val = compile_expr(last)
            emit("  sp_StrArray_push(" + tmp_arr + ", " + val + ");")
          end
        end
      end
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    @in_loop = old
  end

  def compile_select_block(nid)
    @needs_int_array = 1
    @needs_gc = 1
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr_gc_rooted(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array" || rt == "sym_array"
      bp_t = elem_type_of_array(rt)
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    mrb_int lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
      push_scope
      declare_var(bp1, bp_t)
      blk = @nd_block[nid]
      if blk >= 0
        body = @nd_body[blk]
        if body >= 0
          stmts = get_stmts(body)
          if stmts.length > 0
            last = stmts.last
            cond = compile_expr(last)
            emit("  if (" + cond + ") sp_IntArray_push(" + tmp_arr + ", lv_" + bp1 + ");")
          end
        end
      end
      pop_scope
      @indent = @indent - 1
      emit("  }")
    end
    @in_loop = old
  end

  def compile_stmts_body(nid)
    if nid < 0
      return
    end
    stmts = get_stmts(nid)
    k = 0
    while k < stmts.length
      compile_stmt(stmts[k])
      k = k + 1
    end
  end

  def compile_body_return(body_id, return_type)
    if body_id < 0
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    stmts = get_stmts(body_id)
    if stmts.length == 0
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    # All but last
    i = 0
    while i < stmts.length - 1
      compile_stmt(stmts[i])
      i = i + 1
    end
    last = stmts.last
    if @nd_type[last] == "ReturnNode"
      compile_return_stmt(last)
      return
    end
    if @nd_type[last] == "IfNode"
      compile_if_return(last, return_type)
      return
    end
    if @nd_type[last] == "CaseNode"
      compile_case_return(last, return_type)
      return
    end
    if @nd_type[last] == "WhileNode"
      compile_while_stmt(last)
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    if @nd_type[last] == "YieldNode"
      compile_yield_stmt(last)
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    if @nd_type[last] == "BeginNode"
      compile_begin_stmt(last)
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    if @nd_type[last] == "CaseMatchNode"
      compile_case_match_return(last, return_type)
      return
    end
    # If last statement is a CallNode with a block, handle map/select as expressions
    if @nd_type[last] == "CallNode"
      if @nd_block[last] >= 0
        lmname = @nd_name[last]
        if lmname == "map"
          if return_type != "void"
            val = compile_map_expr(last)
            emit("  return " + val + ";")
            return
          end
        end
        if lmname == "select"
          if return_type != "void"
            val = compile_select_expr(last)
            emit("  return " + val + ";")
            return
          end
        end
        if lmname == "sum"
          if return_type != "void"
            rtype_sum = infer_type(@nd_receiver[last])
            rc_sum = compile_expr(@nd_receiver[last])
            val = compile_array_sum_block(last, rc_sum, rtype_sum)
            emit("  return " + val + ";")
            return
          end
        end
        if lmname == "count"
          if return_type != "void"
            rtype_cnt = infer_type(@nd_receiver[last])
            rc_cnt = compile_expr(@nd_receiver[last])
            val = compile_array_count_block(last, rc_cnt, rtype_cnt)
            emit("  return " + val + ";")
            return
          end
        end
        if lmname == "proc"
          if return_type != "void"
            val = compile_proc_literal(last)
            emit("  return " + val + ";")
            return
          end
        end
        compile_stmt(last)
        if return_type != "void"
          emit("  return " + c_return_default(return_type) + ";")
        end
        return
      end
    end
    # For statement-like nodes as last expression, compile as stmt then return default
    lt = @nd_type[last]
    if lt == "CallNode"
      lm = @nd_name[last]
      if lm == "[]=" || lm == "push" || lm == "pop" || lm == "emit" || lm == "emit_raw" || lm == "puts" || lm == "print" || lm == "p" || lm == "printf" || lm == "warn" || lm == "raise" || lm == "exit" || lm == "abort" || lm == "sleep" || lm == "delete" || lm == "clear" || lm == "concat" || lm == "prepend" || lm == "fill" || lm == "insert" || lm == "update" || lm == "merge!" || lm == "store" || lm == "reverse!" || lm == "sort!" || lm == "each" || lm == "times" || lm == "upto" || lm == "downto"
        compile_stmt(last)
        if return_type != "void"
          emit("  return " + c_return_default(return_type) + ";")
        end
        return
      end
      # Receiver-based setter calls (obj.attr = val)
      if lm.end_with?("=") && lm != "==" && lm != "!=" && lm != "<=" && lm != ">="
        compile_stmt(last)
        if return_type != "void"
          emit("  return " + c_return_default(return_type) + ";")
        end
        return
      end
    end
    if lt == "InstanceVariableWriteNode"
      compile_stmt(last)
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    if lt == "InstanceVariableOperatorWriteNode"
      compile_stmt(last)
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    if lt == "GlobalVariableWriteNode"
      compile_stmt(last)
      if return_type != "void"
        emit("  return " + sanitize_gvar(@nd_name[last]) + ";")
      end
      return
    end
    if lt == "LocalVariableWriteNode"
      compile_stmt(last)
      if return_type != "void"
        emit("  return lv_" + @nd_name[last] + ";")
      end
      return
    end
    if lt == "LocalVariableOperatorWriteNode"
      compile_stmt(last)
      if return_type != "void"
        emit("  return lv_" + @nd_name[last] + ";")
      end
      return
    end
    if lt == "MultiWriteNode"
      compile_stmt(last)
      if return_type != "void"
        emit("  return " + c_return_default(return_type) + ";")
      end
      return
    end
    if return_type != "void"
      if return_type == "poly"
        ret_expr = box_expr_to_poly(last)
        if @in_gc_scope == 1
          tmp = new_temp
          emit("  sp_RbVal " + tmp + " = " + ret_expr + ";")
          emit("  SP_GC_RESTORE();")
          emit("  return " + tmp + ";")
        else
          emit("  return " + ret_expr + ";")
        end
        return
      end
      val = compile_expr(last)
      expr_type = infer_type(last)
      if expr_type == "lambda"
        if return_type == "int"
          emit("  return sp_lam_to_int(" + val + ");")
        else
          if return_type == "bool"
            emit("  return (" + val + ")->u.bval;")
          else
            emit("  return " + val + ";")
          end
        end
      else
        emit("  return " + val + ";")
      end
    else
      compile_stmt(last)
    end
    return
  end

  def compile_if_return(nid, rt)
    cond = compile_cond_expr(@nd_predicate[nid])
    emit("  if (" + cond + ") {")
    @indent = @indent + 1
    body = @nd_body[nid]
    if body >= 0
      compile_body_return(body, rt)
    else
      if rt != "void"
        emit("  return " + c_return_default(rt) + ";")
      end
    end
    @indent = @indent - 1
    sub = @nd_subsequent[nid]
    if sub >= 0
      if @nd_type[sub] == "ElseNode"
        emit("  } else {")
        @indent = @indent + 1
        eb = @nd_body[sub]
        if eb >= 0
          compile_body_return(eb, rt)
        else
          if rt != "void"
            emit("  return " + c_return_default(rt) + ";")
          end
        end
        @indent = @indent - 1
      else
        emit("  } else")
        compile_if_return(sub, rt)
        return
      end
    else
      if rt != "void"
        emit("  } else {")
        emit("    return " + c_return_default(rt) + ";")
      end
    end
    emit("  }")
  end

  def compile_case_match_return(nid, rt)
    pred = @nd_predicate[nid]
    pred_type = infer_type(pred)
    pred_val = compile_expr(pred)
    tmp = new_temp
    if pred_type == "poly"
      emit("  sp_RbVal " + tmp + " = " + pred_val + ";")
    else
      if pred_type == "string"
        emit("  const char *" + tmp + " = " + pred_val + ";")
      else
        if pred_type == "float"
          emit("  mrb_float " + tmp + " = " + pred_val + ";")
        else
          emit("  mrb_int " + tmp + " = " + pred_val + ";")
        end
      end
    end
    conds = parse_id_list(@nd_conditions[nid])
    k = 0
    while k < conds.length
      inid = conds[k]
      if @nd_type[inid] == "InNode"
        kw = "if"
        if k > 0
          kw = "} else if"
        end
        pat = @nd_pattern[inid]
        cond_str = compile_in_pattern(pat, tmp, pred_type)
        emit("  " + kw + " (" + cond_str + ") {")
        @indent = @indent + 1
        compile_body_return(@nd_body[inid], rt)
        @indent = @indent - 1
      end
      k = k + 1
    end
    ec = @nd_else_clause[nid]
    if ec >= 0
      emit("  } else {")
      @indent = @indent + 1
      compile_body_return(@nd_body[ec], rt)
      @indent = @indent - 1
    end
    if conds.length > 0
      emit("  }")
    end
  end

  def compile_case_return(nid, rt)
    pred = @nd_predicate[nid]
    if pred >= 0
      pred_type = infer_type(pred)
      pred_val = compile_expr(pred)
      tmp = new_temp
      if pred_type == "string"
        emit("  const char *" + tmp + " = " + pred_val + ";")
      else
        emit("  mrb_int " + tmp + " = " + pred_val + ";")
      end
      conds = parse_id_list(@nd_conditions[nid])
      k = 0
      while k < conds.length
        wid = conds[k]
        if @nd_type[wid] == "WhenNode"
          kw = "if"
          if k > 0
            kw = "} else if"
          end
          cond_str = compile_when_conds(wid, tmp, pred_type)
          emit("  " + kw + " (" + cond_str + ") {")
          @indent = @indent + 1
          compile_body_return(@nd_body[wid], rt)
          @indent = @indent - 1
        end
        k = k + 1
      end
    else
      conds = parse_id_list(@nd_conditions[nid])
      k = 0
      while k < conds.length
        wid = conds[k]
        kw = "if"
        if k > 0
          kw = "} else if"
        end
        wconds = parse_id_list(@nd_conditions[wid])
        cexpr = "0"
        if wconds.length > 0
          cexpr = compile_expr(wconds.first)
        end
        emit("  " + kw + " (" + cexpr + ") {")
        @indent = @indent + 1
        compile_body_return(@nd_body[wid], rt)
        @indent = @indent - 1
        k = k + 1
      end
    end
    ec = @nd_else_clause[nid]
    if ec >= 0
      emit("  } else {")
      @indent = @indent + 1
      compile_body_return(@nd_body[ec], rt)
      @indent = @indent - 1
    end
    emit("  }")
  end
end

# ---- Main ----
ast_file = ARGV[0]
out_file = ARGV[1]

if ast_file == nil
  $stderr.puts "Usage: ruby spinel_codegen.rb ast.txt output.c"
  exit(1)
end

data = File.read(ast_file)
compiler = Compiler.new
compiler.read_text_ast(data)
compiler.compile

result = compiler.build_output
if out_file != nil
  File.write(out_file, result)
else
  print result
end
