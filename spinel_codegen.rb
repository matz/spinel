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
    @out = ""
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

    @nd_count = 0
    @root_id = 0

    # ---- Top-level methods (parallel arrays) ----
    @meth_names = "".split(",")
    @meth_param_names = "".split(",")
    @meth_param_types = "".split(",")
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
    @cls_attr_readers = "".split(",")
    @cls_attr_writers = "".split(",")
    @cls_cmeth_names = "".split(",")
    @cls_cmeth_params = "".split(",")
    @cls_cmeth_ptypes = "".split(",")
    @cls_cmeth_returns = "".split(",")
    @cls_cmeth_bodies = "".split(",")
    @cls_is_value_type = []

    # ---- Constants (parallel arrays) ----
    @const_names = "".split(",")
    @const_types = "".split(",")
    @const_expr_ids = []

    # ---- Scope stack for local variables ----
    @scope_names = "".split(",")
    @scope_types = "".split(",")

    @current_class_idx = -1
    @current_method_name = ""
    @current_method_return = ""
    @in_main = 0
    @in_loop = 0
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
    @needs_ptr_array = 0
    @needs_str_array = 0
    @needs_str_int_hash = 0
    @needs_str_str_hash = 0
    @needs_string_helpers = 0
    @needs_setjmp = 0
    @needs_file_io = 0
    @needs_mutable_str = 0
    @needs_rb_value = 0
    @needs_regexp = 0
    @regexp_patterns = "".split(",")
    @regexp_flags = "".split(",")
    @needs_stringio = 0
    @needs_proc = 0
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
  end

  # Backslash-n for C string literals - bootstrap-safe (avoids escape level issues)
  def bsl_n
    92.chr + "n"
  end

  # Backslash for C char literals - bootstrap-safe
  def bsl
    92.chr
  end


  # Parse comma-sep node IDs into IntArray
  def parse_id_list(s)
    if s == ""
      return []
    end
    parts = s.split(",")
    result = []
    i = 0
    while i < parts.length
      result.push(parts[i].to_i)
      i = i + 1
    end
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
    @out = @out + ind + s + 10.chr
  end

  def emit_raw(s)
    @out = @out + s + 10.chr
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
      return "string"
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
      ci = find_const_idx(@nd_name[nid])
      if ci >= 0
        return @const_types[ci]
      end
      cx = find_class_idx(@nd_name[nid])
      if cx >= 0
        return "class_" + @nd_name[nid]
      end
      # Check module-prefixed constants
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          cpname = mmod + "_" + @nd_name[nid]
          ci4 = find_const_idx(cpname)
          if ci4 >= 0
            return @const_types[ci4]
          end
        end
        mi3 = mi3 + 1
      end
      return "int"
    end
    if t == "ConstantPathNode"
      if @nd_receiver[nid] >= 0
        rname = @nd_name[@nd_receiver[nid]]
        nname = @nd_name[nid]
        cpname = rname + "_" + nname
        ci = find_const_idx(cpname)
        if ci >= 0
          return @const_types[ci]
        end
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
      body = @nd_body[nid]
      if body >= 0
        stmts = get_stmts(body)
        if stmts.length > 0
          return infer_type(stmts.last)
        end
      end
      return "void"
    end
    if t == "CaseMatchNode"
      # Infer from first in branch
      conds = parse_id_list(@nd_conditions[nid])
      if conds.length > 0
        inid = conds.first
        if @nd_type[inid] == "InNode"
          ibody = @nd_body[inid]
          if ibody >= 0
            is = get_stmts(ibody)
            if is.length > 0
              return infer_type(is.last)
            end
          end
        end
      end
      return "int"
    end
    if t == "CaseNode"
      # Infer from first when branch
      conds = parse_id_list(@nd_conditions[nid])
      if conds.length > 0
        wid = conds.first
        if @nd_type[wid] == "WhenNode"
          wbody = @nd_body[wid]
          if wbody >= 0
            ws = get_stmts(wbody)
            if ws.length > 0
              return infer_type(ws.last)
            end
          end
        end
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
        if all_same == 1
          if first_vt == "string"
            return "str_str_hash"
          end
        else
          # Mixed value types - store as str_str_hash with auto-conversion
          return "str_str_hash"
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
        if @nd_type[recv] == "ConstantReadNode"
          if @nd_name[recv] == "Proc"
            return "proc"
          end
          if @nd_name[recv] == "Fiber"
            return "fiber"
          end
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
        if @nd_type[recv] == "ConstantReadNode"
          if @nd_name[recv] == "Fiber"
            return "poly"
          end
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
        if @nd_type[recv] == "ConstantReadNode"
          if @nd_name[recv] == "Fiber"
            return "fiber"
          end
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
    # Bigint operators return bigint
    if recv >= 0
      lt = infer_type(recv)
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
        lt = infer_type(recv)
        if lt == "string"
          return "string"
        end
        if lt == "mutable_str"
          return "string"
        end
        if lt == "poly"
          return "poly"
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
        lt = infer_type(recv)
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
        lt = infer_type(recv)
        if lt == "float"
          return "float"
        end
        if lt == "string"
          return "string"
        end
        if lt == "poly"
          return "poly"
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
        lt = infer_type(recv)
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
        lt = infer_type(recv)
        if lt == "mutable_str"
          return "mutable_str"
        end
      end
      return "int"
    end
    if mname == "%"
      return "int"
    end
    if mname == "-@"
      if recv >= 0
        return infer_type(recv)
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
    if mname == "itself"
      if recv >= 0
        return infer_type(recv)
      end
      return "int"
    end
    if mname == "succ"
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
    if mname == "to_sym"
      return "string"
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
    if mname == "**"
      if recv >= 0
        lt = infer_type(recv)
        if lt == "float"
          return "float"
        end
      end
      return "int"
    end
    if mname == "has_key?" || mname == "key?"
      return "bool"
    end
    if mname == "split"
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
    if mname == "index"
      return "int"
    end
    if mname == "keys"
      return "str_array"
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
      end
      return "int"
    end
    if mname == "shift"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
      end
      return "int"
    end
    if mname == "sort"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "first"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
      end
      return "int"
    end
    if mname == "last"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "str_array"
          return "string"
        end
      end
      return "int"
    end
    if mname == "min"
      return "int"
    end
    if mname == "max"
      return "int"
    end
    if mname == "sum"
      return "int"
    end
    if mname == "reverse"
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
      return "int_array"
    end
    if mname == "reject"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "map"
      if recv >= 0
        # Check block body return type to determine result array type
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
            end
          end
        end
        return infer_type(recv)
      end
      return "int_array"
    end
    if mname == "select"
      if recv >= 0
        return infer_type(recv)
      end
      return "int_array"
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
          return "int"
        end
        if rt == "float_array"
          return "float"
        end
        if rt == "str_array"
          return "string"
        end
        if is_ptr_array_type(rt) == 1
          return ptr_array_elem_type(rt)
        end
        if rt == "str_int_hash"
          return "int"
        end
        if rt == "str_str_hash"
          return "string"
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
        if @nd_type[recv] == "ConstantReadNode"
          rn = @nd_name[recv]
          if rn == "Array"
            # Check fill value type
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
      if @nd_type[recv] == "ConstantReadNode"
        rcname = @nd_name[recv]
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
    if mname == "log"
      return "float"
    end
    if mname == "exp"
      return "float"
    end
    if mname == "atan2"
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
        # Check all classes for this method and return the first matching return type
        ci = 0
        while ci < @cls_names.length
          midx = cls_find_method_direct(ci, mname)
          if midx >= 0
            return cls_method_return(ci, mname)
          end
          ci = ci + 1
        end
        return "int"
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
  def ptr_array_elem_type(t)
    if is_ptr_array_type(t) == 1
      return t[0, t.length - 10]
    end
    ""
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
    if t == "lambda"
      return 1
    end
    if t == "mutable_str"
      return 1
    end
    if t == "fiber" || t == "bigint"
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
    if bt == "int_array" || bt == "str_array" || bt == "float_array"
      return 1
    end
    if bt == "str_int_hash" || bt == "str_str_hash"
      return 1
    end
    if bt == "stringio" || bt == "lambda" || bt == "poly_array"
      return 1
    end
    if bt == "fiber" || bt == "bigint"
      return 1
    end
    if is_obj_type(bt) == 1
      return 1
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
    if t == "fiber"
      return "sp_Fiber *"
    end
    if t == "poly"
      return "sp_RbVal"
    end
    if t == "proc"
      return "sp_Proc"
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
      return "\"\""
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
      return "sp_proc_new(NULL)"
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
    if name.length > 0
      if name[0] == "@"
        return name[1, name.length - 1]
      end
    end
    name
  end

  def sanitize_gvar(name)
    # $last → gv_last, $1 → gv_1
    if name.length > 0 && name[0] == "$"
      return "gv_" + name[1, name.length - 1]
    end
    "gv_" + name
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
    ci = @cls_names.length
    cname = ""
    cp = @nd_constant_path[nid]
    if cp >= 0
      cname = @nd_name[cp]
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
            @meth_return_types.push("int")
            @meth_body_ids.push(@nd_body[sid])
            @meth_has_yield.push(0)
            @meth_has_defaults.push("0")
          end
        }
      end
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
    else
      @cls_meth_names.push("")
      @cls_meth_params.push("")
      @cls_meth_ptypes.push("")
      @cls_meth_returns.push("")
      @cls_meth_bodies.push("")
      @cls_meth_defaults.push("")
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
      if @nd_type[sid] == "CallNode"
        cn = @nd_name[sid]
        if cn != "include"
          if cn != "private"
            collect_attr_call(ci, sid)
          end
        end
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
        result = result + @nd_name[blk]
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
    else
      @cls_meth_names[ci] = name
      @cls_meth_params[ci] = params
      @cls_meth_ptypes[ci] = ptypes
      @cls_meth_returns[ci] = ret
      @cls_meth_bodies[ci] = body_id.to_s
      @cls_meth_defaults[ci] = defaults
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
      return "string"
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
          if @nd_type[r] == "ConstantReadNode"
            rname = @nd_name[r]
            if rname == "Array"
              return "int_array"
            end
            if rname == "Hash"
              return "str_int_hash"
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
    @meth_return_types.push("int")
    @meth_body_ids.push(body_id)
    @meth_has_defaults.push("")
    @meth_has_yield.push(0)
  end

  def collect_module(nid)
    mname = ""
    cp = @nd_constant_path[nid]
    if cp >= 0
      mname = @nd_name[cp]
    end
    body = @nd_body[nid]
    # Store module info for include
    @module_names.push(mname)
    @module_body_ids.push(body)
    if body < 0
      return
    end
    body_stmts = get_stmts(body)
    body_stmts.each { |sid|
      if @nd_type[sid] == "ConstantWriteNode"
        cname = mname + "_" + @nd_name[sid]
        expr_id = @nd_expression[sid]
        ct = "int"
        if expr_id >= 0
          ct = infer_type(expr_id)
        end
        @const_names.push(cname)
        @const_types.push(ct)
        @const_expr_ids.push(expr_id)
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
          ct = infer_type(expr_id)
        end
        @const_names.push(cname2)
        @const_types.push(ct)
        @const_expr_ids.push(expr_id)
      end
    }
  end

  def collect_constant(nid)
    # Check for Struct.new(:x, :y)
    expr_id = @nd_expression[nid]
    if expr_id >= 0
      if @nd_type[expr_id] == "CallNode"
        if @nd_name[expr_id] == "new"
          sr = @nd_receiver[expr_id]
          if sr >= 0
            if @nd_type[sr] == "ConstantReadNode"
              if @nd_name[sr] == "Struct"
                collect_struct_class(@nd_name[nid], expr_id)
                return
              end
            end
          end
        end
      end
    end
    @const_names.push(@nd_name[nid])
    ct = "int"
    if expr_id >= 0
      ct = infer_type(expr_id)
    end
    @const_types.push(ct)
    @const_expr_ids.push(expr_id)
  end

  def collect_struct_class(cname, call_nid)
    # Generate a synthetic class from Struct.new(:field1, :field2, ...)
    ci = @cls_names.length
    @cls_names.push(cname)
    @cls_is_value_type.push(0)
    @cls_parents.push("")
    @cls_ivar_names.push("")
    @cls_ivar_types.push("")
    @cls_meth_names.push("")
    @cls_meth_params.push("")
    @cls_meth_ptypes.push("")
    @cls_meth_returns.push("")
    @cls_meth_bodies.push("")
    @cls_meth_defaults.push("")
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
                at = infer_type(arg_ids[ak])
                if ak < ptypes.length
                  if ptypes[ak] == "int"
                    if at != "int"
                      ptypes[ak] = at
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
          if @nd_type[recv] == "ConstantReadNode"
            cname = @nd_name[recv]
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
                                at = infer_type(@nd_expression[elems[ek]])
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
                          at = infer_type(arg_ids[k])
                          if k < ptypes.length
                            old_pt = ptypes[k]
                            if old_pt == "int"
                              if at != "int"
                                ptypes[k] = at
                              end
                            elsif old_pt == "nil" && at != "nil" && at != "int"
                              if is_nullable_pointer_type(at) == 1
                                ptypes[k] = at + "?"
                              else
                                ptypes[k] = at
                              end
                            elsif at == "nil" && old_pt != "nil" && is_nullable_pointer_type(old_pt) == 1
                              if old_pt[old_pt.length - 1] != "?"
                                ptypes[k] = old_pt + "?"
                              end
                            end
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
                  # Check against ALL classes' readers and zero-arg methods
                  ci2 = 0
                  found_class = 0
                  while ci2 < @cls_names.length
                    if found_class == 0
                      readers = @cls_attr_readers[ci2].split(";")
                      if readers.length > 0
                        if param_calls_reader(bid, pnames[pk], readers) == 1
                          found_class = 1
                        end
                      end
                      # Also check zero-arg class methods as readers
                      if found_class == 0
                        ci2_mnames = @cls_meth_names[ci2].split(";")
                        ci2_mparams = @cls_meth_params[ci2].split("|")
                        zero_arg_meths = "".split(",")
                        mi2 = 0
                        while mi2 < ci2_mnames.length
                          mn2 = ci2_mnames[mi2]
                          if mn2 != "initialize"
                            mp2 = ""
                            if mi2 < ci2_mparams.length
                              mp2 = ci2_mparams[mi2]
                            end
                            if mp2 == ""
                              zero_arg_meths.push(mn2)
                            end
                          end
                          mi2 = mi2 + 1
                        end
                        if zero_arg_meths.length > 0
                          if param_calls_reader(bid, pnames[pk], zero_arg_meths) == 1
                            found_class = 1
                          end
                        end
                      end
                      if found_class == 1
                        ptypes[pk] = "obj_" + @cls_names[ci2]
                        all_ptypes[j] = ptypes.join(",")
                        @cls_meth_ptypes[oci] = all_ptypes.join("|")
                      end
                    end
                    ci2 = ci2 + 1
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
    # Also infer top-level method param types from body usage
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
              ci2 = 0
              found_class = 0
              while ci2 < @cls_names.length
                if found_class == 0
                  readers = @cls_attr_readers[ci2].split(";")
                  if readers.length > 0
                    if param_calls_reader(bid, pnames[pk], readers) == 1
                      found_class = 1
                    end
                  end
                  if found_class == 0
                    ci2_mnames = @cls_meth_names[ci2].split(";")
                    ci2_mparams = @cls_meth_params[ci2].split("|")
                    zero_arg_meths = "".split(",")
                    mi2 = 0
                    while mi2 < ci2_mnames.length
                      mn2 = ci2_mnames[mi2]
                      if mn2 != "initialize"
                        mp2 = ""
                        if mi2 < ci2_mparams.length
                          mp2 = ci2_mparams[mi2]
                        end
                        if mp2 == ""
                          zero_arg_meths.push(mn2)
                        end
                      end
                      mi2 = mi2 + 1
                    end
                    if zero_arg_meths.length > 0
                      if param_calls_reader(bid, pnames[pk], zero_arg_meths) == 1
                        found_class = 1
                      end
                    end
                  end
                  if found_class == 1
                    ptypes[pk] = "obj_" + @cls_names[ci2]
                    @meth_param_types[mi] = ptypes.join(",")
                  end
                end
                ci2 = ci2 + 1
              end
            end
          end
          pk = pk + 1
        end
      end
      mi = mi + 1
    end
  end

  def param_calls_reader(nid, pname, readers)
    if nid < 0
      return 0
    end
    if @nd_type[nid] == "CallNode"
      recv = @nd_receiver[nid]
      if recv >= 0
        if @nd_type[recv] == "LocalVariableReadNode"
          if @nd_name[recv] == pname
            mname = @nd_name[nid]
            ri = 0
            while ri < readers.length
              if readers[ri] == mname
                return 1
              end
              ri = ri + 1
            end
          end
        end
      end
    end
    # Recurse
    if @nd_body[nid] >= 0
      if param_calls_reader(@nd_body[nid], pname, readers) == 1
        return 1
      end
    end
    stmts = parse_id_list(@nd_stmts[nid])
    k = 0
    while k < stmts.length
      if param_calls_reader(stmts[k], pname, readers) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_expression[nid] >= 0
      if param_calls_reader(@nd_expression[nid], pname, readers) == 1
        return 1
      end
    end
    if @nd_left[nid] >= 0
      if param_calls_reader(@nd_left[nid], pname, readers) == 1
        return 1
      end
    end
    if @nd_right[nid] >= 0
      if param_calls_reader(@nd_right[nid], pname, readers) == 1
        return 1
      end
    end
    if @nd_arguments[nid] >= 0
      if param_calls_reader(@nd_arguments[nid], pname, readers) == 1
        return 1
      end
    end
    args = parse_id_list(@nd_args[nid])
    k = 0
    while k < args.length
      if param_calls_reader(args[k], pname, readers) == 1
        return 1
      end
      k = k + 1
    end
    if @nd_receiver[nid] >= 0
      if param_calls_reader(@nd_receiver[nid], pname, readers) == 1
        return 1
      end
    end
    0
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
        at = infer_type(@nd_expression[nid])
        if at != "int" && at != "nil"
          update_ivar_type(@current_class_idx, iname, at)
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
            pt = infer_init_param_type(i, pnames[k])
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
    # Check for explicit returns
    rt = scan_return_type_nid(body_id)
    if rt != ""
      return rt
    end
    # Last expression
    infer_type(stmts.last)
  end

  def scan_return_type_nid(nid)
    stmts = get_stmts(nid)
    i = 0
    while i < stmts.length
      rt = scan_return_type(stmts[i])
      if rt != ""
        return rt
      end
      i = i + 1
    end
    ""
  end

  def scan_return_type(nid)
    if nid < 0
      return ""
    end
    if @nd_type[nid] == "ReturnNode"
      args_id = @nd_arguments[nid]
      if args_id >= 0
        arg_ids = get_args(args_id)
        if arg_ids.length > 0
          return infer_type(arg_ids[0])
        end
      end
      return "void"
    end
    if @nd_type[nid] == "IfNode"
      r1 = ""
      body = @nd_body[nid]
      if body >= 0
        r1 = scan_return_type_nid(body)
      end
      sub = @nd_subsequent[nid]
      if sub >= 0
        r2 = scan_return_type(sub)
        if r2 != ""
          r1 = r2
        end
      end
      return r1
    end
    if @nd_type[nid] == "ElseNode"
      body = @nd_body[nid]
      if body >= 0
        return scan_return_type_nid(body)
      end
    end
    if @nd_type[nid] == "WhileNode"
      body = @nd_body[nid]
      if body >= 0
        return scan_return_type_nid(body)
      end
    end
    if @nd_type[nid] == "CaseMatchNode"
      conds = parse_id_list(@nd_conditions[nid])
      if conds.length > 0
        inid = conds.first
        if @nd_type[inid] == "InNode"
          ibody = @nd_body[inid]
          if ibody >= 0
            rt = scan_return_type_nid(ibody)
            if rt != ""
              return rt
            end
          end
        end
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
    if t == "InterpolatedXStringNode"
      @needs_file_io = 1
      @needs_string_helpers = 1
    end
    if t == "RegularExpressionNode"
      @needs_regexp = 1
      # Collect pattern and flags
      pat = @nd_unescaped[nid]
      flags = "0"
      if @nd_flags[nid] != 0
        f = @nd_flags[nid]
        parts = "".split(",")
        if f & 1 != 0
          parts.push("1")
        end
        if f & 2 != 0
          parts.push("6")
        end
        if f & 8 != 0
          parts.push("8")
        end
        if parts.length > 0
          flags = parts.join("|")
        end
      end
      @regexp_patterns.push(pat)
      @regexp_flags.push(flags)
    end
    if t == "XStringNode"
      @needs_file_io = 1
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
      else
        @needs_str_int_hash = 1
      end
      @needs_gc = 1
      @needs_str_array = 1
    end
    if t == "InterpolatedStringNode"
      @needs_string_helpers = 1
    end
    if t == "SymbolNode"
      @needs_string_helpers = 1
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
      if mname == "to_s"
        @needs_string_helpers = 1
      end
      if mname == "+"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_string_helpers = 1
          end
        end
      end
      if mname == "upcase"
        @needs_string_helpers = 1
      end
      if mname == "<<"
        @needs_string_helpers = 1
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_string_helpers = 1
          end
        end
      end
      if mname == "+"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_string_helpers = 1
          end
        end
      end
      if mname == "downcase"
        @needs_string_helpers = 1
      end
      if mname == "strip"
        @needs_string_helpers = 1
      end
      if mname == "chomp"
        @needs_string_helpers = 1
      end
      if mname == "slice"
        @needs_string_helpers = 1
      end
      if mname == "include?"
        @needs_string_helpers = 1
      end
      if mname == "start_with?"
        @needs_string_helpers = 1
      end
      if mname == "end_with?"
        @needs_string_helpers = 1
      end
      if mname == "split"
        @needs_string_helpers = 1
        @needs_str_array = 1
        @needs_gc = 1
      end
      if mname == "gsub"
        @needs_string_helpers = 1
      end
      if mname == "index"
        @needs_string_helpers = 1
      end
      if mname == "sub"
        @needs_string_helpers = 1
      end
      if mname == "tr"
        @needs_string_helpers = 1
      end
      if mname == "ljust"
        @needs_string_helpers = 1
      end
      if mname == "rjust"
        @needs_string_helpers = 1
      end
      if mname == "[]"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_string_helpers = 1
          end
        end
      end
      if mname == "reverse"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_string_helpers = 1
          end
        end
      end
      if mname == "capitalize"
        @needs_string_helpers = 1
      end
      if mname == "count"
        @needs_string_helpers = 1
      end
      if mname == "*"
        if @nd_receiver[nid] >= 0
          rt = infer_type(@nd_receiver[nid])
          if rt == "string"
            @needs_string_helpers = 1
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
            if rn == "Proc"
              @needs_proc = 1
            end
            if rn == "StringIO"
              @needs_stringio = 1
            end
          end
        end
      end
      if mname == "proc"
        @needs_proc = 1
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
        @needs_file_io = 1
        @needs_system = 1
      end
      if @nd_receiver[nid] >= 0
        if @nd_type[@nd_receiver[nid]] == "ConstantReadNode"
          rn = @nd_name[@nd_receiver[nid]]
          if rn == "File"
            @needs_file_io = 1
            if mname == "join"
              @needs_string_helpers = 1
            end
            if mname == "basename"
              @needs_string_helpers = 1
            end
          end
        end
      end
      if mname == "keys"
        @needs_str_array = 1
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
        end
      end
    end
    if t == "BlockParameterNode"
      @needs_proc = 1
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
      targets.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode"
          lname = @nd_name[tid]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              types.push("int")
            end
          end
        end
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

  def compile
    collect_all
    infer_main_call_types
    infer_function_body_call_types
    infer_class_body_call_types
    detect_poly_locals
    # Iterative type inference: converge param types, return types, ivar types
    iter = 0
    while iter < 4
      infer_all_returns
      infer_ivar_types_from_writers
      detect_poly_params
      iter = iter + 1
    end
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
    emit_header
    if @needs_gc == 1
      emit_gc_runtime
    end
    if @needs_int_array == 1
      emit_int_array_runtime
    end
    if @needs_float_array == 1
      if @needs_gc == 0
        @needs_gc = 1
        emit_gc_runtime
      end
      emit_float_array_runtime
    end
    if @needs_ptr_array == 1
      if @needs_gc == 0
        @needs_gc = 1
        emit_gc_runtime
      end
      emit_ptr_array_runtime
    end
    if @needs_str_array == 1
      emit_str_array_runtime
    end
    if @needs_str_int_hash == 1 || @needs_str_str_hash == 1
      emit_str_hash_func
    end
    if @needs_str_int_hash == 1
      emit_str_int_hash_runtime
    end
    if @needs_str_str_hash == 1
      emit_str_str_hash_runtime
    end
    if @needs_string_helpers == 1
      # String helpers need StrArray (for split), IntArray (for bytes), and GC
      if @needs_gc == 0
        @needs_gc = 1
        emit_gc_runtime
      end
      if @needs_int_array == 0
        @needs_int_array = 1
        emit_int_array_runtime
      end
      if @needs_str_array == 0
        @needs_str_array = 1
        emit_str_array_runtime
      end
      emit_string_helpers
    end
    if @needs_mutable_str == 1
      if @needs_gc == 0
        @needs_gc = 1
        emit_gc_runtime
      end
      if @needs_string_helpers == 0
        @needs_string_helpers = 1
        emit_string_helpers
      end
      emit_mutable_str_runtime
    end
    if @needs_regexp == 1
      if @needs_string_helpers == 0
        @needs_string_helpers = 1
        emit_string_helpers
      end
      emit_regexp_runtime
    end
    if @needs_rb_value == 1
      if @needs_string_helpers == 0
        @needs_string_helpers = 1
        if @needs_gc == 0
          @needs_gc = 1
          emit_gc_runtime
        end
        if @needs_int_array == 0
          @needs_int_array = 1
          emit_int_array_runtime
        end
        if @needs_str_array == 0
          @needs_str_array = 1
          emit_str_array_runtime
        end
        emit_string_helpers
      end
      emit_rb_value_runtime
    end
    if @needs_setjmp == 1
      emit_setjmp_runtime
    end
    if @needs_file_io == 1
      emit_file_io_runtime
    end
    if @needs_proc == 1
      emit_proc_runtime
    end
    if @needs_stringio == 1
      emit_stringio_runtime
    end
    if @needs_lambda == 1
      emit_lambda_runtime
    end
    if @needs_fiber == 1
      if @needs_gc == 0
        @needs_gc = 1
        emit_gc_runtime
      end
      if @needs_setjmp == 0
        @needs_setjmp = 1
        emit_setjmp_runtime
      end
      if @needs_rb_value == 0
        @needs_rb_value = 1
        if @needs_string_helpers == 0
          @needs_string_helpers = 1
          if @needs_int_array == 0
            @needs_int_array = 1
            emit_int_array_runtime
          end
          if @needs_str_array == 0
            @needs_str_array = 1
            emit_str_array_runtime
          end
          emit_string_helpers
        end
        emit_rb_value_runtime
      end
      emit_fiber_runtime
    end
    if @needs_bigint == 1
      emit_raw("/* Bigint (linked from sp_bigint.o) */")
      emit_raw("typedef struct sp_Bigint sp_Bigint;")
      emit_raw("sp_Bigint *sp_bigint_new_int(int64_t v);")
      emit_raw("sp_Bigint *sp_bigint_new_str(const char *s, int base);")
      emit_raw("sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);")
      emit_raw("sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);")
      emit_raw("sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);")
      emit_raw("sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);")
      emit_raw("sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);")
      emit_raw("sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);")
      emit_raw("int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);")
      emit_raw("int64_t sp_bigint_to_int(sp_Bigint *b);")
      emit_raw("const char *sp_bigint_to_s(sp_Bigint *b);")
      emit_raw("void sp_bigint_free(sp_Bigint *b);")
      emit_raw("")
    end
    emit_class_structs
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
    # Lambda functions will be inserted here (before class/toplevel methods)
    @lambda_insert_pos = @out.length
    emit_class_methods
    emit_toplevel_methods
    # Emit lambda functions before main (they are generated during compilation)
    # We emit them in emit_main after forward declarations
    emit_main
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

  # Check if multiplication x = a * b has unbounded growth (self-referential via assigns)
  def mul_is_unbounded(lname, expr)
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
      if expr >= 0 && @nd_type[expr] == "CallNode" && @nd_name[expr] == "*"
        if mul_is_unbounded(lname, expr) == 1
          if not_in(lname, bigint_names) == 1
            bigint_names.push(lname)
          end
        end
      end
    end
    if @nd_type[nid] == "LocalVariableOperatorWriteNode"
      if @nd_binop[nid] == "*"
        lname = @nd_name[nid]
        if not_in(lname, bigint_names) == 1
          bigint_names.push(lname)
        end
      end
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

  def emit_gc_runtime
    emit_raw("typedef struct sp_gc_hdr { struct sp_gc_hdr *next; void (*finalize)(void *); void (*scan)(void *); size_t size; unsigned marked : 1; } sp_gc_hdr;")
    emit_raw("static sp_gc_hdr *sp_gc_heap = NULL; static size_t sp_gc_bytes = 0; static size_t sp_gc_threshold = 256*1024;")
    emit_raw("#define SP_GC_STACK_MAX 65536")
    emit_raw("static void **sp_gc_roots[SP_GC_STACK_MAX]; static int sp_gc_nroots = 0;")
    emit_raw("#define SP_GC_SAVE() int __attribute__((cleanup(sp_gc_cleanup))) _gc_saved = sp_gc_nroots")
    emit_raw("#define SP_GC_ROOT(v) do{if(sp_gc_nroots<SP_GC_STACK_MAX)sp_gc_roots[sp_gc_nroots++]=(void**)&(v);}while(0)")
    emit_raw("#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved")
    # stack_bottom removed (no conservative scan)
    emit_raw("#define SP_GC_MARK_STACK_MAX (1024*64)")
    emit_raw("static void**sp_gc_mark_stack=NULL;static int sp_gc_mark_top=0;")
    emit_raw("static void sp_gc_mark(void*obj){if(!obj)return;sp_gc_hdr*h=(sp_gc_hdr*)((char*)obj-sizeof(sp_gc_hdr));if(h->marked)return;h->marked=1;if(h->scan){if(sp_gc_mark_stack&&sp_gc_mark_top<SP_GC_MARK_STACK_MAX){sp_gc_mark_stack[sp_gc_mark_top++]=obj;}else{h->scan(obj);}}}")
    emit_raw("static void sp_gc_mark_all(void){if(!sp_gc_mark_stack)sp_gc_mark_stack=(void**)malloc(sizeof(void*)*SP_GC_MARK_STACK_MAX);sp_gc_mark_top=0;for(int i=0;i<sp_gc_nroots;i++){void*obj=*sp_gc_roots[i];if(obj)sp_gc_mark(obj);}while(sp_gc_mark_top>0){void*obj=sp_gc_mark_stack[--sp_gc_mark_top];sp_gc_hdr*h=(sp_gc_hdr*)((char*)obj-sizeof(sp_gc_hdr));if(h->scan)h->scan(obj);}}")
    emit_raw("static void sp_gc_cleanup(int*p){sp_gc_nroots=*p;}")
    # Size-segregated free lists (no arena overhead during sweep)
    emit_raw("#define SP_GC_NBUCKETS 32")
    emit_raw("static sp_gc_hdr*sp_gc_buckets[SP_GC_NBUCKETS];")
    emit_raw("static inline int sp_gc_bucket(size_t sz){int b=(int)(sz/16);return b<SP_GC_NBUCKETS?b:SP_GC_NBUCKETS-1;}")
    emit_raw("static int sp_gc_cycle=0;")
    emit_raw("static sp_gc_hdr*sp_gc_old_heap=NULL;static size_t sp_gc_old_bytes=0;")
    emit_raw("#define SP_GC_FULL_INTERVAL 8")
    emit_raw("static void sp_gc_collect(void){int full=(sp_gc_cycle%SP_GC_FULL_INTERVAL==0);sp_gc_cycle++;sp_gc_hdr*hh=sp_gc_old_heap;while(hh){hh->marked=0;hh=hh->next;}sp_gc_mark_all();if(full){sp_gc_hdr**pp=&sp_gc_old_heap;sp_gc_old_bytes=0;while(*pp){sp_gc_hdr*h=*pp;if(!h->marked){*pp=h->next;if(h->finalize)h->finalize((char*)h+sizeof(sp_gc_hdr));int b=sp_gc_bucket(h->size);h->next=sp_gc_buckets[b];sp_gc_buckets[b]=h;}else{h->marked=1;sp_gc_old_bytes+=h->size;pp=&h->next;}}}else{hh=sp_gc_old_heap;while(hh){hh->marked=1;hh=hh->next;}}sp_gc_hdr**pp=&sp_gc_heap;sp_gc_bytes=sp_gc_old_bytes;while(*pp){sp_gc_hdr*h=*pp;if(!h->marked){*pp=h->next;if(h->finalize)h->finalize((char*)h+sizeof(sp_gc_hdr));int b=sp_gc_bucket(h->size);h->next=sp_gc_buckets[b];sp_gc_buckets[b]=h;}else{h->marked=1;*pp=h->next;h->next=sp_gc_old_heap;sp_gc_old_heap=h;sp_gc_old_bytes+=h->size;sp_gc_bytes+=h->size;}}}")
    emit_raw("static size_t sp_gc_threshold_init=256*1024;")
    gc_linkage = @needs_bigint == 1 ? "" : "static "
    emit_raw(gc_linkage + "void*sp_gc_alloc(size_t sz,void(*fin)(void*),void(*scn)(void*)){if(sp_gc_bytes>sp_gc_threshold){size_t before=sp_gc_bytes;sp_gc_collect();size_t freed=before-sp_gc_bytes;if(freed<before/4){sp_gc_threshold=before*2;}else if(sp_gc_bytes>0){sp_gc_threshold=sp_gc_bytes*4;if(sp_gc_threshold<sp_gc_threshold_init)sp_gc_threshold=sp_gc_threshold_init;}else{sp_gc_threshold=sp_gc_threshold_init;}}size_t need=sizeof(sp_gc_hdr)+sz;int b=sp_gc_bucket(need);sp_gc_hdr*h=NULL;if(sp_gc_buckets[b]&&sp_gc_buckets[b]->size==need){h=sp_gc_buckets[b];sp_gc_buckets[b]=h->next;}if(!h){h=(sp_gc_hdr*)calloc(1,need);}h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}")
    if @needs_bigint == 1
      emit_raw("void*sp_gc_alloc_nogc(size_t sz,void(*fin)(void*),void(*scn)(void*)){size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}")
    end
    emit_raw("")
  end

  def emit_int_array_runtime
    emit_raw("typedef struct{mrb_int*data;mrb_int start;mrb_int len;mrb_int cap;}sp_IntArray;")
    emit_raw("static void sp_IntArray_fin(void*p){free(((sp_IntArray*)p)->data);}")
    emit_raw("static sp_IntArray*sp_IntArray_new(void){sp_IntArray*a=(sp_IntArray*)sp_gc_alloc(sizeof(sp_IntArray),sp_IntArray_fin,NULL);a->cap=16;a->data=(mrb_int*)malloc(sizeof(mrb_int)*a->cap);a->start=0;a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}return a;}")
    emit_raw("static sp_IntArray*sp_IntArray_from_range(mrb_int s,mrb_int e){sp_IntArray*a=sp_IntArray_new();mrb_int n=e-s+1;if(n<0)n=0;if(n>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=n;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}for(mrb_int i=0;i<n;i++)a->data[i]=s+i;a->len=n;return a;}")
    emit_raw("static sp_IntArray*sp_IntArray_dup(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();if(a->len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*b->cap;h->size-=sizeof(mrb_int)*b->cap;b->cap=a->len;b->data=(mrb_int*)realloc(b->data,sizeof(mrb_int)*b->cap);h->size+=sizeof(mrb_int)*b->cap;sp_gc_bytes+=sizeof(mrb_int)*b->cap;}memcpy(b->data,a->data+a->start,sizeof(mrb_int)*a->len);b->len=a->len;return b;}")
    emit_raw("static void __attribute__((noinline)) sp_IntArray_push_grow(sp_IntArray*a){if(a->start>0){memmove(a->data,a->data+a->start,sizeof(mrb_int)*a->len);a->start=0;if(a->len<a->cap)return;}a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);}")
    emit_raw("static inline void sp_IntArray_push(sp_IntArray*a,mrb_int v){if(a->start+a->len>=a->cap)sp_IntArray_push_grow(a);a->data[a->start+a->len]=v;a->len++;}")
    emit_raw("static inline mrb_int sp_IntArray_pop(sp_IntArray*a){return a->data[a->start+--a->len];}")
    emit_raw("static inline mrb_int sp_IntArray_shift(sp_IntArray*a){mrb_int v=a->data[a->start];a->start++;a->len--;return v;}")
    emit_raw("static inline mrb_int sp_IntArray_length(sp_IntArray*a){return a->len;}")
    emit_raw("static inline mrb_bool sp_IntArray_empty(sp_IntArray*a){return a->len==0;}")
    emit_raw("static inline mrb_int sp_IntArray_get(sp_IntArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[a->start+i];}")
    emit_raw("static void sp_IntArray_set_slow(sp_IntArray*a,mrb_int i,mrb_int v){while(a->start+i>=a->cap){a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);}while(i>=a->len){a->data[a->start+a->len]=0;a->len++;}a->data[a->start+i]=v;}")
    emit_raw("static inline void sp_IntArray_set(sp_IntArray*a,mrb_int i,mrb_int v){if(i<0)i+=a->len;if(i>=0&&i<a->len){a->data[a->start+i]=v;return;}sp_IntArray_set_slow(a,i,v);}")
    emit_raw("static void sp_IntArray_reverse_bang(sp_IntArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}")
    emit_raw("static int _sp_int_cmp(const void*a,const void*b){mrb_int va=*(const mrb_int*)a,vb=*(const mrb_int*)b;return(va>vb)-(va<vb);}")
    emit_raw("static sp_IntArray*sp_IntArray_sort(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);qsort(b->data+b->start,b->len,sizeof(mrb_int),_sp_int_cmp);return b;}")
    emit_raw("static void sp_IntArray_sort_bang(sp_IntArray*a){qsort(a->data+a->start,a->len,sizeof(mrb_int),_sp_int_cmp);}")
    emit_raw("static mrb_int sp_IntArray_min(sp_IntArray*a){mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]<m)m=a->data[a->start+i];return m;}")
    emit_raw("static mrb_int sp_IntArray_max(sp_IntArray*a){mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]>m)m=a->data[a->start+i];return m;}")
    emit_raw("static mrb_int sp_IntArray_sum(sp_IntArray*a){mrb_int s=0;for(mrb_int i=0;i<a->len;i++)s+=a->data[a->start+i];return s;}")
    emit_raw("static mrb_bool sp_IntArray_include(sp_IntArray*a,mrb_int v){for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return TRUE;return FALSE;}")
    emit_raw("static sp_IntArray*sp_IntArray_uniq(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){int found=0;for(mrb_int j=0;j<b->len;j++){if(b->data[b->start+j]==a->data[a->start+i]){found=1;break;}}if(!found)sp_IntArray_push(b,a->data[a->start+i]);}return b;}")
    emit_raw("static void sp_IntArray_unshift(sp_IntArray*a,mrb_int v){if(a->start>0){a->start--;a->data[a->start]=v;a->len++;}else{mrb_int e=a->len+1;if(e>a->cap){a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);}memmove(a->data+1,a->data,sizeof(mrb_int)*a->len);a->data[0]=v;a->len++;}}")
    emit_raw("static const char*sp_IntArray_join(sp_IntArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}char tmp[32];int n=snprintf(tmp,32,\"%lld\",(long long)a->data[a->start+i]);if(len+n>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,tmp,n);len+=n;}buf[len]=0;return buf;}")
    emit_raw("static mrb_bool sp_IntArray_eq(sp_IntArray*a,sp_IntArray*b){if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]!=b->data[b->start+i])return FALSE;return TRUE;}")
    emit_raw("")
  end

  def emit_ptr_array_runtime
    emit_raw("/* ---- PtrArray: array of void* pointers ---- */")
    emit_raw("typedef struct{void**data;mrb_int len;mrb_int cap;}sp_PtrArray;")
    emit_raw("static void sp_PtrArray_fin(void*p){free(((sp_PtrArray*)p)->data);}")
    emit_raw("static sp_PtrArray*sp_PtrArray_new(void){sp_PtrArray*a=(sp_PtrArray*)sp_gc_alloc(sizeof(sp_PtrArray),sp_PtrArray_fin,NULL);a->cap=16;a->data=(void**)malloc(sizeof(void*)*a->cap);a->len=0;return a;}")
    emit_raw("static inline void sp_PtrArray_push(sp_PtrArray*a,void*v){if(a->len>=a->cap){a->cap=a->cap*2+1;a->data=(void**)realloc(a->data,sizeof(void*)*a->cap);}a->data[a->len++]=v;}")
    emit_raw("static inline void*sp_PtrArray_get(sp_PtrArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}")
    emit_raw("static inline void sp_PtrArray_set(sp_PtrArray*a,mrb_int i,void*v){if(i<0)i+=a->len;a->data[i]=v;}")
    emit_raw("static inline mrb_int sp_PtrArray_length(sp_PtrArray*a){return a->len;}")
    emit_raw("static inline mrb_bool sp_PtrArray_empty(sp_PtrArray*a){return a->len==0;}")
    emit_raw("")
  end

  def emit_float_array_runtime
    emit_raw("typedef struct{mrb_float*data;mrb_int len;mrb_int cap;}sp_FloatArray;")
    emit_raw("static void sp_FloatArray_fin(void*p){free(((sp_FloatArray*)p)->data);}")
    emit_raw("static sp_FloatArray*sp_FloatArray_new(void){sp_FloatArray*a=(sp_FloatArray*)sp_gc_alloc(sizeof(sp_FloatArray),sp_FloatArray_fin,NULL);a->cap=16;a->data=(mrb_float*)malloc(sizeof(mrb_float)*a->cap);a->len=0;return a;}")
    emit_raw("static inline void sp_FloatArray_push(sp_FloatArray*a,mrb_float v){if(a->len>=a->cap){a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);}a->data[a->len++]=v;}")
    emit_raw("static inline mrb_float sp_FloatArray_pop(sp_FloatArray*a){return a->data[--a->len];}")
    emit_raw("static inline mrb_int sp_FloatArray_length(sp_FloatArray*a){return a->len;}")
    emit_raw("static inline mrb_bool sp_FloatArray_empty(sp_FloatArray*a){return a->len==0;}")
    emit_raw("static inline mrb_float sp_FloatArray_get(sp_FloatArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}")
    emit_raw("static inline void sp_FloatArray_set(sp_FloatArray*a,mrb_int i,mrb_float v){if(i<0)i+=a->len;while(i>=a->cap){a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);}while(i>=a->len){a->data[a->len]=0.0;a->len++;}a->data[i]=v;}")
    emit_raw("")
  end

  def emit_str_array_runtime
    emit_raw("typedef struct{const char**data;mrb_int len;mrb_int cap;}sp_StrArray;")
    emit_raw("static void sp_StrArray_fin(void*p){free(((sp_StrArray*)p)->data);}")
    emit_raw("static sp_StrArray*sp_StrArray_new(void){sp_StrArray*a=(sp_StrArray*)sp_gc_alloc(sizeof(sp_StrArray),sp_StrArray_fin,NULL);a->cap=16;a->data=(const char**)malloc(sizeof(const char*)*a->cap);a->len=0;return a;}")
    emit_raw("static inline void sp_StrArray_push(sp_StrArray*a,const char*v){if(a->len>=a->cap){a->cap=a->cap*2+1;a->data=(const char**)realloc(a->data,sizeof(const char*)*a->cap);}a->data[a->len++]=v;}")
    emit_raw("static const char*sp_StrArray_pop(sp_StrArray*a){return a->data[--a->len];}")
    emit_raw("static inline mrb_int sp_StrArray_length(sp_StrArray*a){return a->len;}")
    emit_raw("static inline mrb_bool sp_StrArray_empty(sp_StrArray*a){return a->len==0;}")
    emit_raw("static inline const char*sp_StrArray_get(sp_StrArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}")
    emit_raw("static inline void sp_StrArray_set(sp_StrArray*a,mrb_int i,const char*v){if(i<0)i+=a->len;while(i>=a->len)sp_StrArray_push(a,\"\");a->data[i]=v;}")
    emit_raw("static const char*sp_StrArray_join(sp_StrArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}size_t el=strlen(a->data[i]);if(len+el>=cap){cap=(len+el)*2+1;buf=(char*)realloc(buf,cap);}memcpy(buf+len,a->data[i],el);len+=el;}buf[len]=0;return buf;}")
    emit_raw("static mrb_bool sp_StrArray_include(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return TRUE;return FALSE;}")
    emit_raw("")
  end

  def emit_str_hash_func
    emit_raw("static inline uint64_t sp_str_hash(const char*s){uint64_t h=14695981039346656037ULL;while(*s){h^=(unsigned char)*s++;h*=1099511628211ULL;}return h;}")
  end

  def emit_str_int_hash_runtime
    emit_raw("typedef struct{const char**keys;mrb_int*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;}sp_StrIntHash;")
    emit_raw("static void sp_StrIntHash_fin(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;free(h->keys);free(h->vals);free(h->order);}")
    emit_raw("static sp_StrIntHash*sp_StrIntHash_new(void){sp_StrIntHash*h=(sp_StrIntHash*)sp_gc_alloc(sizeof(sp_StrIntHash),sp_StrIntHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;return h;}")
    emit_raw("static void sp_StrIntHash_grow(sp_StrIntHash*h){mrb_int oc=h->cap;const char**ok=h->keys;mrb_int*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);mrb_int ol=h->len;h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}")
    emit_raw("static mrb_int sp_StrIntHash_get(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return 0;}")
    emit_raw("static void sp_StrIntHash_set(sp_StrIntHash*h,const char*k,mrb_int v){if(h->len*2>=h->cap)sp_StrIntHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}")
    emit_raw("static mrb_bool sp_StrIntHash_has_key(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}")
    emit_raw("static mrb_int sp_StrIntHash_length(sp_StrIntHash*h){return h->len;}")
    emit_raw("static void sp_StrIntHash_delete(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->keys[idx]=NULL;h->vals[idx]=0;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=0;idx=j;}j=(j+1)&h->mask;}return;}idx=(idx+1)&h->mask;}}")
    emit_raw("static sp_StrArray*sp_StrIntHash_keys(sp_StrIntHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}")
    emit_raw("static sp_StrIntHash*sp_StrIntHash_merge(sp_StrIntHash*a,sp_StrIntHash*b){sp_StrIntHash*r=sp_StrIntHash_new();for(mrb_int i=0;i<a->len;i++)sp_StrIntHash_set(r,a->order[i],sp_StrIntHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(r,b->order[i],sp_StrIntHash_get(b,b->order[i]));return r;}")
    emit_raw("")
  end

  def emit_str_str_hash_runtime
    emit_raw("typedef struct{const char**keys;const char**vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;}sp_StrStrHash;")
    emit_raw("static void sp_StrStrHash_fin(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;free(h->keys);free(h->vals);free(h->order);}")
    emit_raw("static sp_StrStrHash*sp_StrStrHash_new(void){sp_StrStrHash*h=(sp_StrStrHash*)sp_gc_alloc(sizeof(sp_StrStrHash),sp_StrStrHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;return h;}")
    emit_raw("static void sp_StrStrHash_grow(sp_StrStrHash*h){mrb_int oc=h->cap;const char**ok=h->keys;const char**ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}")
    emit_raw("static const char*sp_StrStrHash_get(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return\"\";}")
    emit_raw("static void sp_StrStrHash_set(sp_StrStrHash*h,const char*k,const char*v){if(h->len*2>=h->cap)sp_StrStrHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}")
    emit_raw("static mrb_bool sp_StrStrHash_has_key(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}")
    emit_raw("static mrb_int sp_StrStrHash_length(sp_StrStrHash*h){return h->len;}")
    emit_raw("static void sp_StrStrHash_delete(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->keys[idx]=NULL;h->vals[idx]=NULL;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=NULL;idx=j;}j=(j+1)&h->mask;}return;}idx=(idx+1)&h->mask;}}")
    emit_raw("static sp_StrArray*sp_StrStrHash_keys(sp_StrStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}")
    emit_raw("")
  end

  def emit_string_helpers
    emit_raw("static const char*sp_str_concat(const char*a,const char*b){size_t la=strlen(a),lb=strlen(b);char*r=(char*)malloc(la+lb+1);memcpy(r,a,la);memcpy(r+la,b,lb+1);return r;}")
    emit_raw("static const char*sp_int_to_s(mrb_int n){char*b=(char*)malloc(32);snprintf(b,32,\"%lld\",(long long)n);return b;}")
    emit_raw("static const char*sp_float_to_s(mrb_float f){char*b=(char*)malloc(64);snprintf(b,64,\"%g\",f);return b;}")
    emit_raw("static const char*sp_str_upcase(const char*s){size_t l=strlen(s);char*r=(char*)malloc(l+1);for(size_t i=0;i<=l;i++)r[i]=toupper((unsigned char)s[i]);return r;}")
    emit_raw("static const char*sp_str_downcase(const char*s){size_t l=strlen(s);char*r=(char*)malloc(l+1);for(size_t i=0;i<=l;i++)r[i]=tolower((unsigned char)s[i]);return r;}")
    emit_raw("static const char*sp_str_strip(const char*s){while(*s&&isspace((unsigned char)*s))s++;size_t l=strlen(s);while(l>0&&isspace((unsigned char)s[l-1]))l--;char*r=(char*)malloc(l+1);memcpy(r,s,l);r[l]=0;return r;}")
    emit_raw("static const char*sp_str_chomp(const char*s){size_t l=strlen(s);while(l>0&&(s[l-1]=='" + bsl_n + "'||s[l-1]=='" + bsl + "r'))l--;char*r=(char*)malloc(l+1);memcpy(r,s,l);r[l]=0;return r;}")
    emit_raw("static mrb_bool sp_str_include(const char*s,const char*sub){return strstr(s,sub)!=NULL;}")
    emit_raw("static mrb_bool sp_str_start_with(const char*s,const char*p){return strncmp(s,p,strlen(p))==0;}")
    emit_raw("static mrb_bool sp_str_end_with(const char*s,const char*suf){size_t ls=strlen(s),lsuf=strlen(suf);if(lsuf>ls)return FALSE;return strcmp(s+ls-lsuf,suf)==0;}")
    emit_raw("static sp_StrArray*sp_str_split(const char*s,const char*sep){sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;size_t sl=strlen(sep);if(sl==0){for(size_t i=0;s[i];i++){char*c=(char*)malloc(2);c[0]=s[i];c[1]=0;sp_StrArray_push(a,c);}return a;}const char*p=s;while(1){const char*f=strstr(p,sep);if(!f){char*r=(char*)malloc(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);break;}size_t n=f-p;char*r=(char*)malloc(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);p=f+sl;}return a;}")
    emit_raw("static const char*sp_str_gsub(const char*s,const char*pat,const char*rep){size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);if(pl==0)return s;size_t cap=sl*2+1;char*out=(char*)malloc(cap);size_t ol=0;const char*p=s;while(*p){const char*f=strstr(p,pat);if(!f){size_t n=strlen(p);if(ol+n>=cap){cap=(ol+n)*2+1;out=(char*)realloc(out,cap);}memcpy(out+ol,p,n);ol+=n;break;}size_t n=f-p;if(ol+n+rl>=cap){cap=(ol+n+rl)*2+1;out=(char*)realloc(out,cap);}memcpy(out+ol,p,n);ol+=n;memcpy(out+ol,rep,rl);ol+=rl;p=f+pl;}out[ol]=0;return out;}")
    emit_raw("static mrb_int sp_str_index(const char*s,const char*sub){const char*f=strstr(s,sub);if(!f)return -1;return(mrb_int)(f-s);}")
    emit_raw("static const char*sp_str_sub_range(const char*s,mrb_int start,mrb_int len){mrb_int sl=(mrb_int)strlen(s);if(start<0)start+=sl;if(start<0)start=0;if(start>=sl)return\"\";if(len<0)len=0;if(start+len>sl)len=sl-start;char*r=(char*)malloc(len+1);memcpy(r,s+start,len);r[len]=0;return r;}")
    emit_raw("static const char*sp_sprintf(const char*fmt,...){char*b=(char*)malloc(4096);va_list ap;va_start(ap,fmt);vsnprintf(b,4096,fmt,ap);va_end(ap);return b;}")
    emit_raw("static const char*sp_str_reverse(const char*s){size_t l=strlen(s);char*r=(char*)malloc(l+1);for(size_t i=0;i<l;i++)r[i]=s[l-1-i];r[l]=0;return r;}")
    emit_raw("static const char*sp_str_sub(const char*s,const char*pat,const char*rep){const char*f=strstr(s,pat);if(!f)return s;size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);char*r=(char*)malloc(sl-pl+rl+1);size_t n=f-s;memcpy(r,s,n);memcpy(r+n,rep,rl);memcpy(r+n+rl,f+pl,sl-n-pl+1);return r;}")
    emit_raw("static const char*sp_str_capitalize(const char*s){size_t l=strlen(s);char*r=(char*)malloc(l+1);for(size_t i=0;i<=l;i++)r[i]=tolower((unsigned char)s[i]);if(l>0)r[0]=toupper((unsigned char)r[0]);return r;}")
    emit_raw("static mrb_int sp_str_count(const char*s,const char*chars){mrb_int c=0;for(size_t i=0;s[i];i++){for(size_t j=0;chars[j];j++){if(s[i]==chars[j]){c++;break;}}}return c;}")
    emit_raw("static const char*sp_str_repeat(const char*s,mrb_int n){if(n<=0)return\"\";size_t l=strlen(s);char*r=(char*)malloc(l*n+1);for(mrb_int i=0;i<n;i++)memcpy(r+l*i,s,l);r[l*n]=0;return r;}")
    emit_raw("static sp_IntArray*sp_str_bytes(const char*s){sp_IntArray*a=sp_IntArray_new();for(size_t i=0;s[i];i++)sp_IntArray_push(a,(mrb_int)(unsigned char)s[i]);return a;}")
    emit_raw("static const char*sp_str_tr(const char*s,const char*from,const char*to){size_t l=strlen(s),fl=strlen(from),tl=strlen(to);char*r=(char*)malloc(l+1);for(size_t i=0;i<l;i++){r[i]=s[i];for(size_t j=0;j<fl;j++){if(s[i]==from[j]){if(j<tl)r[i]=to[j];else if(tl>0)r[i]=to[tl-1];break;}}}r[l]=0;return r;}")
    emit_raw("static const char*sp_str_delete(const char*s,const char*chars){size_t l=strlen(s);char*r=(char*)malloc(l+1);size_t n=0;for(size_t i=0;i<l;i++){int found=0;for(size_t j=0;chars[j];j++){if(s[i]==chars[j]){found=1;break;}}if(!found)r[n++]=s[i];}r[n]=0;return r;}")
    emit_raw("static const char*sp_str_squeeze(const char*s){size_t l=strlen(s);char*r=(char*)malloc(l+1);size_t n=0;for(size_t i=0;i<l;i++){if(i==0||s[i]!=s[i-1])r[n++]=s[i];}r[n]=0;return r;}")
    emit_raw("static const char*sp_str_ljust(const char*s,mrb_int w){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=(char*)malloc(w+1);memcpy(r,s,l);memset(r+l,' ',w-l);r[w]=0;return r;}")
    emit_raw("static const char*sp_str_rjust(const char*s,mrb_int w){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=(char*)malloc(w+1);memset(r,' ',w-l);memcpy(r+w-l,s,l+1);return r;}")
    emit_raw("static const char*sp_str_center(const char*s,mrb_int w){size_t l=strlen(s);if((mrb_int)l>=w)return s;mrb_int pad=w-l;mrb_int left=pad/2;mrb_int right=pad-left;char*r=(char*)malloc(w+1);memset(r,' ',left);memcpy(r+left,s,l);memset(r+left+l,' ',right);r[w]=0;return r;}")
    emit_raw("static const char*sp_str_ljust2(const char*s,mrb_int w,const char*pad){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=(char*)malloc(w+1);memcpy(r,s,l);char pc=pad[0];for(mrb_int i=l;i<w;i++)r[i]=pc;r[w]=0;return r;}")
    emit_raw("static const char*sp_str_rjust2(const char*s,mrb_int w,const char*pad){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=(char*)malloc(w+1);char pc=pad[0];for(mrb_int i=0;i<w-(mrb_int)l;i++)r[i]=pc;memcpy(r+w-l,s,l+1);return r;}")
    emit_raw("static const char*sp_str_lstrip(const char*s){while(*s&&isspace((unsigned char)*s))s++;char*r=(char*)malloc(strlen(s)+1);strcpy(r,s);return r;}")
    emit_raw("static const char*sp_str_rstrip(const char*s){size_t l=strlen(s);while(l>0&&isspace((unsigned char)s[l-1]))l--;char*r=(char*)malloc(l+1);memcpy(r,s,l);r[l]=0;return r;}")
    emit_raw("static const char*sp_str_dup(const char*s){char*r=(char*)malloc(strlen(s)+1);strcpy(r,s);return r;}")
    emit_raw("")
  end

  def emit_mutable_str_runtime
    emit_raw("typedef struct{char*data;int64_t len;int64_t cap;}sp_String;")
    emit_raw("static void sp_String_fin(void*p){free(((sp_String*)p)->data);}")
    emit_raw("static sp_String*sp_String_new(const char*s){sp_String*r=(sp_String*)sp_gc_alloc(sizeof(sp_String),sp_String_fin,NULL);r->len=(int64_t)strlen(s);r->cap=r->len*2+16;r->data=(char*)malloc(r->cap+1);memcpy(r->data,s,r->len+1);{sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->size+=r->cap+1;sp_gc_bytes+=r->cap+1;}return r;}")
    emit_raw("static inline void sp_String_append(sp_String*s,const char*t){int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));sp_gc_bytes-=s->cap+1;h->size-=s->cap+1;s->cap=(s->len+tl)*2+16;s->data=(char*)realloc(s->data,s->cap+1);h->size+=s->cap+1;sp_gc_bytes+=s->cap+1;}memcpy(s->data+s->len,t,tl+1);s->len+=tl;}")
    emit_raw("static inline const char*sp_String_cstr(sp_String*s){return s->data;}")
    emit_raw("static inline int64_t sp_String_length(sp_String*s){return s->len;}")
    emit_raw("static sp_String*sp_String_dup(sp_String*s){return sp_String_new(s->data);}")
    emit_raw("")
  end

  def emit_regexp_runtime
    # Declarations for the external regexp library (libspre.a)
    emit_raw("/* Regexp engine (link with libspre.a from lib/regexp/) */")
    emit_raw("typedef struct mrb_regexp_pattern mrb_regexp_pattern;")
    emit_raw("mrb_regexp_pattern* re_compile(const char *pattern, int64_t len, uint32_t flags);")
    emit_raw("void re_free(mrb_regexp_pattern *pat);")
    emit_raw("int re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size);")
    emit_raw("")
    emit_raw("/* Regexp globals: $1-$9 captures */")
    emit_raw("static const char *sp_re_captures[10] = {0};")
    emit_raw("static int sp_re_caps[64];")
    emit_raw("static const char *sp_re_last_str = \"\";")
    emit_raw("")
    emit_raw("static void sp_re_set_captures(const char *str, int *caps, int ncaps) {")
    emit_raw("  sp_re_last_str = str;")
    emit_raw("  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;")
    emit_raw("  for (int i = 1; i < ncaps && i < 10; i++) {")
    emit_raw("    if (caps[i*2] >= 0 && caps[i*2+1] >= 0) {")
    emit_raw("      int len = caps[i*2+1] - caps[i*2];")
    emit_raw("      char *buf = (char*)malloc(len+1);")
    emit_raw("      memcpy(buf, str+caps[i*2], len); buf[len] = 0;")
    emit_raw("      sp_re_captures[i] = buf;")
    emit_raw("    }")
    emit_raw("  }")
    emit_raw("}")
    emit_raw("")
    emit_raw("static mrb_int sp_re_match(mrb_regexp_pattern *pat, const char *str) {")
    emit_raw("  int64_t slen = (int64_t)strlen(str);")
    emit_raw("  int ncaps = 32;")
    emit_raw("  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps);")
    emit_raw("  if (n > 0) { sp_re_set_captures(str, sp_re_caps, n/2); return sp_re_caps[0] + 1; }")
    emit_raw("  return 0;")
    emit_raw("}")
    emit_raw("")
    emit_raw("static mrb_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str) {")
    emit_raw("  int64_t slen = (int64_t)strlen(str);")
    emit_raw("  int caps[2];")
    emit_raw("  return re_exec(pat, str, slen, 0, caps, 2) > 0;")
    emit_raw("}")
    emit_raw("")
    emit_raw("static const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep) {")
    emit_raw("  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);")
    emit_raw("  size_t cap = slen * 2 + 64; char *out = (char*)malloc(cap); size_t olen = 0;")
    emit_raw("  int64_t pos = 0; int caps[64];")
    emit_raw("  while (pos <= slen) {")
    emit_raw("    int n = re_exec(pat, str, slen, pos, caps, 64);")
    emit_raw("    if (n <= 0 || caps[0] < 0) break;")
    emit_raw("    size_t before = caps[0] - pos;")
    emit_raw("    if (olen+before+rlen >= cap) { cap = (olen+before+rlen)*2+64; out = (char*)realloc(out, cap); }")
    emit_raw("    memcpy(out+olen, str+pos, before); olen += before;")
    emit_raw("    memcpy(out+olen, rep, rlen); olen += rlen;")
    emit_raw("    pos = caps[1]; if (caps[0] == caps[1]) pos++;")
    emit_raw("  }")
    emit_raw("  size_t rest = slen - pos;")
    emit_raw("  if (olen+rest >= cap) { cap = olen+rest+1; out = (char*)realloc(out, cap); }")
    emit_raw("  memcpy(out+olen, str+pos, rest); olen += rest;")
    emit_raw("  out[olen] = 0; return out;")
    emit_raw("}")
    emit_raw("")
    emit_raw("static const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep) {")
    emit_raw("  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);")
    emit_raw("  int caps[64];")
    emit_raw("  int n = re_exec(pat, str, slen, 0, caps, 64);")
    emit_raw("  if (n <= 0 || caps[0] < 0) return str;")
    emit_raw("  size_t out_len = caps[0] + rlen + (slen - caps[1]);")
    emit_raw("  char *out = (char*)malloc(out_len + 1);")
    emit_raw("  memcpy(out, str, caps[0]);")
    emit_raw("  memcpy(out+caps[0], rep, rlen);")
    emit_raw("  memcpy(out+caps[0]+rlen, str+caps[1], slen-caps[1]+1);")
    emit_raw("  return out;")
    emit_raw("}")
    emit_raw("")
    emit_raw("static sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str) {")
    emit_raw("  sp_StrArray *arr = sp_StrArray_new();")
    emit_raw("  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];")
    emit_raw("  while (pos <= slen) {")
    emit_raw("    int n = re_exec(pat, str, slen, pos, caps, 64);")
    emit_raw("    if (n <= 0 || caps[0] < 0) break;")
    emit_raw("    int len = caps[1] - caps[0];")
    emit_raw("    char *m = (char*)malloc(len+1); memcpy(m, str+caps[0], len); m[len] = 0;")
    emit_raw("    sp_StrArray_push(arr, m);")
    emit_raw("    pos = caps[1]; if (caps[0] == caps[1]) pos++;")
    emit_raw("  }")
    emit_raw("  return arr;")
    emit_raw("}")
    emit_raw("")
    emit_raw("static sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str) {")
    emit_raw("  sp_StrArray *arr = sp_StrArray_new();")
    emit_raw("  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];")
    emit_raw("  while (pos <= slen) {")
    emit_raw("    int n = re_exec(pat, str, slen, pos, caps, 64);")
    emit_raw("    if (n <= 0 || caps[0] < 0) {")
    emit_raw("      int len = slen - pos; char *m = (char*)malloc(len+1);")
    emit_raw("      memcpy(m, str+pos, len); m[len] = 0; sp_StrArray_push(arr, m); break;")
    emit_raw("    }")
    emit_raw("    int len = caps[0] - pos; char *m = (char*)malloc(len+1);")
    emit_raw("    memcpy(m, str+pos, len); m[len] = 0; sp_StrArray_push(arr, m);")
    emit_raw("    pos = caps[1]; if (caps[0] == caps[1]) pos++;")
    emit_raw("  }")
    emit_raw("  return arr;")
    emit_raw("}")
    emit_raw("")
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
        else
          if ch == 34.chr
            cpat = cpat + 92.chr + 34.chr
          else
            cpat = cpat + ch
          end
        end
        pi = pi + 1
      end
      emit_raw("  sp_re_pat_" + i.to_s + " = re_compile(\"" + cpat + "\", " + pat.length.to_s + ", " + flags + ");")
      i = i + 1
    end
    emit_raw("}")
    emit_raw("")
  end

  def emit_rb_value_runtime
    emit_raw("/* NaN-boxed polymorphic value */")
    emit_raw("typedef uint64_t sp_RbValue;")
    emit_raw("#define SP_TAG_INT  0")
    emit_raw("#define SP_TAG_STR  1")
    emit_raw("#define SP_TAG_FLT  2")
    emit_raw("#define SP_TAG_BOOL 3")
    emit_raw("#define SP_TAG_NIL  4")
    emit_raw("#define SP_TAG_OBJ  5")
    emit_raw("typedef struct { int tag; union { mrb_int i; const char *s; mrb_float f; mrb_bool b; void *p; int cls_id; } v; } sp_RbVal;")
    emit_raw("static sp_RbVal sp_box_int(mrb_int v) { sp_RbVal r; r.tag = SP_TAG_INT; r.v.i = v; return r; }")
    emit_raw("static sp_RbVal sp_box_str(const char *v) { sp_RbVal r; r.tag = SP_TAG_STR; r.v.s = v; return r; }")
    emit_raw("static sp_RbVal sp_box_float(mrb_float v) { sp_RbVal r; r.tag = SP_TAG_FLT; r.v.f = v; return r; }")
    emit_raw("static sp_RbVal sp_box_bool(mrb_bool v) { sp_RbVal r; r.tag = SP_TAG_BOOL; r.v.b = v; return r; }")
    emit_raw("static sp_RbVal sp_box_nil(void) { sp_RbVal r; r.tag = SP_TAG_NIL; r.v.i = 0; return r; }")
    emit_raw("static sp_RbVal sp_box_obj(void *p, int cls_id) { sp_RbVal r; r.tag = SP_TAG_OBJ; r.v.p = p; r.v.cls_id = cls_id; return r; }")
    emit_raw("static void sp_poly_puts(sp_RbVal v) {")
    emit_raw("  switch (v.tag) {")
    emit_raw("    case SP_TAG_INT: printf(\"%lld" + bsl_n + "\", (long long)v.v.i); break;")
    emit_raw("    case SP_TAG_STR: if (v.v.s) { fputs(v.v.s, stdout); if (!*v.v.s || v.v.s[strlen(v.v.s)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); break;")
    emit_raw("    case SP_TAG_FLT: { char _fb[64]; snprintf(_fb,64,\"%g\",v.v.f); if(!strchr(_fb,'.')&&!strchr(_fb,'e')&&!strchr(_fb,'i')&&!strchr(_fb,'n')){strcat(_fb,\".0\");} printf(\"%s" + bsl_n + "\",_fb); break; }")
    emit_raw("    case SP_TAG_BOOL: puts(v.v.b ? \"true\" : \"false\"); break;")
    emit_raw("    case SP_TAG_NIL: putchar('" + bsl_n + "'); break;")
    emit_raw("    default: printf(\"%lld" + bsl_n + "\", (long long)v.v.i); break;")
    emit_raw("  }")
    emit_raw("}")
    emit_raw("static mrb_bool sp_poly_nil_p(sp_RbVal v) { return v.tag == SP_TAG_NIL; }")
    emit_raw("static const char *sp_poly_to_s(sp_RbVal v) { switch (v.tag) { case SP_TAG_INT: { char *b = (char*)malloc(32); snprintf(b, 32, \"%lld\", (long long)v.v.i); return b; } case SP_TAG_STR: return v.v.s ? v.v.s : \"\"; case SP_TAG_FLT: { char *b = (char*)malloc(64); snprintf(b, 64, \"%g\", v.v.f); return b; } case SP_TAG_BOOL: return v.v.b ? \"true\" : \"false\"; case SP_TAG_NIL: return \"\"; default: return \"\"; } }")
    emit_raw("static sp_RbVal sp_poly_add(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i + b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f + b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i + b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f + (mrb_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) return sp_box_str(sp_str_concat(a.v.s, b.v.s)); return sp_box_int(0); }")
    emit_raw("static sp_RbVal sp_poly_sub(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i - b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f - b.v.f); return sp_box_int(0); }")
    emit_raw("static sp_RbVal sp_poly_mul(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i * b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f * b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i * b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f * (mrb_float)b.v.i); return sp_box_int(0); }")
    emit_raw("static mrb_bool sp_poly_gt(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return a.v.i > b.v.i; if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return a.v.f > b.v.f; return FALSE; }")
    emit_raw("")
    emit_raw("/* PolyArray: array of sp_RbVal */")
    emit_raw("typedef struct { sp_RbVal *data; mrb_int len; mrb_int cap; } sp_PolyArray;")
    emit_raw("static sp_PolyArray *sp_PolyArray_new(void) { sp_PolyArray *a = (sp_PolyArray *)sp_gc_alloc(sizeof(sp_PolyArray), NULL, NULL); a->cap = 16; a->data = (sp_RbVal *)malloc(sizeof(sp_RbVal) * a->cap); a->len = 0; return a; }")
    emit_raw("static void sp_PolyArray_push(sp_PolyArray *a, sp_RbVal v) { if (a->len >= a->cap) { a->cap = a->cap * 2 + 1; a->data = (sp_RbVal *)realloc(a->data, sizeof(sp_RbVal) * a->cap); } a->data[a->len++] = v; }")
    emit_raw("static mrb_int sp_PolyArray_length(sp_PolyArray *a) { return a->len; }")
    emit_raw("static sp_RbVal sp_PolyArray_get(sp_PolyArray *a, mrb_int i) { if (i < 0) i += a->len; return a->data[i]; }")
    emit_raw("")
  end

  def emit_setjmp_runtime
    emit_raw("#include <setjmp.h>")
    emit_raw("#define SP_EXC_STACK_MAX 64")
    emit_raw("static jmp_buf sp_exc_stack[SP_EXC_STACK_MAX];")
    emit_raw("static const char *sp_exc_msg[SP_EXC_STACK_MAX];")
    emit_raw("static volatile int sp_exc_top = 0;")
    emit_raw("static const char *sp_exc_cls[SP_EXC_STACK_MAX];")
    emit_raw("static volatile const char *sp_last_exc_cls = \"\";")
    emit_raw("static void sp_raise_cls(const char *cls, const char *msg) { if (sp_exc_top > 0) { sp_exc_msg[sp_exc_top-1] = msg; sp_exc_cls[sp_exc_top-1] = cls; sp_last_exc_cls = cls; longjmp(sp_exc_stack[sp_exc_top-1], 1); } fprintf(stderr, \"unhandled exception: %s" + bsl_n + "\", msg); exit(1); }")
    emit_raw("static void sp_raise(const char *msg) { sp_raise_cls(\"RuntimeError\", msg); }")
    emit_raw("static mrb_bool sp_exc_is_a(const char *cls, const char *target) { return strcmp(cls, target) == 0; }")
    emit_raw("")
    # catch/throw support
    emit_raw("#define SP_CATCH_STACK_MAX 64")
    emit_raw("static jmp_buf sp_catch_stack[SP_CATCH_STACK_MAX];")
    emit_raw("static const char *sp_catch_tag[SP_CATCH_STACK_MAX];")
    emit_raw("static mrb_int sp_catch_val[SP_CATCH_STACK_MAX];")
    emit_raw("static volatile int sp_catch_top = 0;")
    emit_raw("static void sp_throw(const char *tag, mrb_int val) { int i = sp_catch_top - 1; while (i >= 0) { if (strcmp(sp_catch_tag[i], tag) == 0) { sp_catch_val[i] = val; sp_catch_top = i + 1; longjmp(sp_catch_stack[i], 1); } i--; } fprintf(stderr, \"uncaught throw: %s" + bsl_n + "\", tag); exit(1); }")
    emit_raw("")
  end

  def emit_file_io_runtime
    emit_raw("static const char *sp_file_read(const char *path) { FILE *f = fopen(path, \"rb\"); if (!f) return \"\"; fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); char *buf = (char *)malloc(sz + 1); if (sz > 0) { size_t r = fread(buf, 1, sz, f); (void)r; } buf[sz] = 0; fclose(f); return buf; }")
    emit_raw("static void sp_file_write(const char *path, const char *data) { FILE *f = fopen(path, \"w\"); if (f) { fputs(data, f); fclose(f); } }")
    emit_raw("static mrb_bool sp_file_exist(const char *path) { FILE *f = fopen(path, \"r\"); if (f) { fclose(f); return TRUE; } return FALSE; }")
    emit_raw("static void sp_file_delete(const char *path) { remove(path); }")
    emit_raw("static const char *sp_backtick(const char *cmd) { FILE *p = popen(cmd, \"r\"); if (!p) return \"\"; char *buf = (char *)malloc(4096); size_t n = fread(buf, 1, 4095, p); buf[n] = 0; pclose(p); return buf; }")
    emit_raw("static const char *sp_file_basename(const char *path) { const char *s = strrchr(path, '/'); if (s) return s + 1; return path; }")
    emit_raw("")
  end

  def emit_proc_runtime
    emit_raw("typedef mrb_int (*sp_proc_fn_t)(mrb_int);")
    emit_raw("typedef struct { sp_proc_fn_t fn; } sp_Proc;")
    emit_raw("static sp_Proc sp_proc_new(sp_proc_fn_t fn) { sp_Proc p; p.fn = fn; return p; }")
    emit_raw("static mrb_int sp_proc_call(sp_Proc p, mrb_int arg) { return p.fn ? p.fn(arg) : 0; }")
    emit_raw("")
  end

  def emit_lambda_runtime
    emit_raw("/* ---- Lambda/closure runtime (sp_Val) ---- */")
    emit_raw("#include <sys/mman.h>")
    emit_raw("typedef struct sp_Val sp_Val;")
    emit_raw("typedef sp_Val *(*sp_fn_t)(sp_Val *self, sp_Val *arg);")
    emit_raw("struct sp_Val { enum { SP_PROC2, SP_INT2, SP_BOOL2, SP_NIL2 } tag; union { struct { sp_fn_t fn; int ncaptures; } proc; mrb_int ival; mrb_bool bval; } u; sp_Val *captures[]; };")
    emit_raw("#define SP_ARENA_SIZE ((size_t)16ULL * 1024 * 1024 * 1024)")
    emit_raw("static char *sp_arena = NULL; static size_t sp_arena_pos = 0;")
    emit_raw("static void *sp_lam_alloc(size_t sz) { sz = (sz + 7) & ~(size_t)7; if (!sp_arena) { sp_arena = (char *)mmap(NULL, SP_ARENA_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0); if (sp_arena == MAP_FAILED) { perror(\"mmap\"); exit(1); } sp_arena_pos = 0; } if (sp_arena_pos + sz > SP_ARENA_SIZE) { fprintf(stderr, \"arena exhausted" + bsl_n + "\"); exit(1); } void *p = sp_arena + sp_arena_pos; sp_arena_pos += sz; return p; }")
    emit_raw("static sp_Val *sp_lam_proc(sp_fn_t fn, int ncap) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val) + sizeof(sp_Val *) * ncap); v->tag = SP_PROC2; v->u.proc.fn = fn; v->u.proc.ncaptures = ncap; return v; }")
    emit_raw("static sp_Val *sp_lam_int(mrb_int n) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val)); v->tag = SP_INT2; v->u.ival = n; return v; }")
    emit_raw("static sp_Val *sp_lam_bool(mrb_bool b) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val)); v->tag = SP_BOOL2; v->u.bval = b; return v; }")
    emit_raw("static sp_Val sp_lam_nil_val = { .tag = SP_NIL2 };")
    emit_raw("static sp_Val *sp_lam_call(sp_Val *f, sp_Val *arg) { return f->u.proc.fn(f, arg); }")
    emit_raw("static mrb_int sp_lam_to_int(sp_Val *v) { return v->u.ival; }")
    emit_raw("")
  end

  def emit_fiber_runtime
    emit_raw("/* ---- Fiber runtime (ucontext) ---- */")
    emit_raw("#include <ucontext.h>")
    emit_raw("#include <sys/mman.h>")
    emit_raw("#define SP_FIBER_STACK_SIZE (64*1024)")
    emit_raw("typedef struct sp_Fiber{ucontext_t ctx;ucontext_t caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;}sp_Fiber;")
    emit_raw("static sp_Fiber sp_fiber_root;")
    emit_raw("static sp_Fiber*sp_fiber_current=&sp_fiber_root;")
    emit_raw("static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->stack)munmap(f->stack,SP_FIBER_STACK_SIZE);}")
    emit_raw("static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);}")
    emit_raw("static sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->stack=(char*)mmap(NULL,SP_FIBER_STACK_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;return f;}")
    emit_raw("static void sp_fiber_trampoline(void){sp_Fiber*f=sp_fiber_current;f->body(f);f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;setcontext(&sp_fiber_root.ctx);}else{swapcontext(&f->ctx,&f->caller_ctx);}}")
    emit_raw("static sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls(\"FiberError\",\"attempt to resume a terminated fiber\");}f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_current=f;if(f->state==0){f->state=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&f->caller_ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&f->caller_ctx,&f->ctx);}else{f->state=1;swapcontext(&f->caller_ctx,&f->ctx);}sp_fiber_current=prev;return f->yielded_value;}")
    emit_raw("static sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;f->yielded_value=val;f->state=2;swapcontext(&f->ctx,&f->caller_ctx);return f->resumed_value;}")
    emit_raw("static mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}")
    emit_raw("static sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&prev->ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&prev->ctx,&f->ctx);}else{f->state=1;swapcontext(&prev->ctx,&f->ctx);}sp_fiber_current=prev;return prev->resumed_value;}")
    emit_raw("")
  end

  def emit_stringio_runtime
    emit_raw("/* ---- StringIO runtime ---- */")
    emit_raw("typedef struct { char *buf; int64_t len; int64_t cap; int64_t pos; int64_t lineno; int closed; } sp_StringIO;")
    emit_raw("static void sio_grow(sp_StringIO *sio, int64_t need) { int64_t req = sio->pos + need; if (req <= sio->cap) return; int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2; sio->buf = (char *)realloc(sio->buf, nc + 1); sio->cap = nc; }")
    emit_raw("static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) { sio_grow(sio, dl); if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len); memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl; if (sio->pos > sio->len) sio->len = sio->pos; sio->buf[sio->len] = '" + bsl + "0'; return dl; }")
    emit_raw("static sp_StringIO *sp_StringIO_new(void) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); s->buf = (char *)calloc(1, 64); s->cap = 63; return s; }")
    emit_raw("static sp_StringIO *sp_StringIO_new_s(const char *init) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); int64_t l = (int64_t)strlen(init); int64_t c = l < 63 ? 63 : l; s->buf = (char *)malloc(c+1); memcpy(s->buf, init, l); s->buf[l]='" + bsl + "0'; s->len = l; s->cap = c; return s; }")
    emit_raw("static const char *sp_StringIO_string(sp_StringIO *s) { return s->buf ? s->buf : \"\"; }")
    emit_raw("static int64_t sp_StringIO_pos(sp_StringIO *s) { return s->pos; }")
    emit_raw("static int64_t sp_StringIO_size(sp_StringIO *s) { return s->len; }")
    emit_raw("static int64_t sp_StringIO_write(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }")
    emit_raw("static int64_t sp_StringIO_puts(sp_StringIO *s, const char *str) { int64_t l = (int64_t)strlen(str); sio_write(s, str, l); if (l == 0 || str[l-1] != '" + bsl_n + "') sio_write(s, \"" + bsl_n + "\", 1); return 0; }")
    emit_raw("static int64_t sp_StringIO_puts_empty(sp_StringIO *s) { sio_write(s, \"" + bsl_n + "\", 1); return 0; }")
    emit_raw("static int64_t sp_StringIO_print(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }")
    emit_raw("static int64_t sp_StringIO_putc(sp_StringIO *s, int64_t ch) { char c = (char)(ch & 0xFF); sio_write(s, &c, 1); return ch; }")
    emit_raw("static const char *sp_StringIO_read(sp_StringIO *s) { if (s->pos >= s->len) return \"\"; const char *r = s->buf + s->pos; s->pos = s->len; return r; }")
    emit_raw("static const char *sp_StringIO_read_n(sp_StringIO *s, int64_t n) { if (s->pos >= s->len) return \"\"; int64_t rem = s->len - s->pos; if (n > rem) n = rem; char *r = (char *)malloc(n+1); memcpy(r, s->buf + s->pos, n); r[n] = '" + bsl + "0'; s->pos += n; return r; }")
    emit_raw("static const char *sp_StringIO_gets(sp_StringIO *s) { if (s->pos >= s->len) return NULL; const char *st = s->buf + s->pos; const char *nl = memchr(st, '" + bsl_n + "', s->len - s->pos); int64_t ll = nl ? (nl - st) + 1 : s->len - s->pos; char *r = (char *)malloc(ll+1); memcpy(r, st, ll); r[ll] = '" + bsl + "0'; s->pos += ll; s->lineno++; return r; }")
    emit_raw("static const char *sp_StringIO_getc(sp_StringIO *s) { if (s->pos >= s->len) return NULL; char *gc = (char *)malloc(2); gc[0] = s->buf[s->pos++]; gc[1] = '" + bsl + "0'; return gc; }")
    emit_raw("static int64_t sp_StringIO_getbyte(sp_StringIO *s) { if (s->pos >= s->len) return -1; return (int64_t)(unsigned char)s->buf[s->pos++]; }")
    emit_raw("static int64_t sp_StringIO_rewind(sp_StringIO *s) { s->pos = 0; s->lineno = 0; return 0; }")
    emit_raw("static int64_t sp_StringIO_seek(sp_StringIO *s, int64_t off) { if (off < 0) off = 0; s->pos = off; return 0; }")
    emit_raw("static int64_t sp_StringIO_tell(sp_StringIO *s) { return s->pos; }")
    emit_raw("static mrb_bool sp_StringIO_eof_p(sp_StringIO *s) { return s->pos >= s->len; }")
    emit_raw("static int64_t sp_StringIO_truncate(sp_StringIO *s, int64_t l) { if (l < 0) l = 0; if (l < s->len) { s->len = l; s->buf[l] = '" + bsl + "0'; } return 0; }")
    emit_raw("static int64_t sp_StringIO_close(sp_StringIO *s) { s->closed = 1; return 0; }")
    emit_raw("static mrb_bool sp_StringIO_closed_p(sp_StringIO *s) { return s->closed; }")
    emit_raw("static sp_StringIO *sp_StringIO_flush(sp_StringIO *s) { return s; }")
    emit_raw("static mrb_bool sp_StringIO_sync(sp_StringIO *s) { (void)s; return 1; }")
    emit_raw("static mrb_bool sp_StringIO_isatty(sp_StringIO *s) { (void)s; return 0; }")
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
    0
  end

  def is_simple_writer_method(mn, bid)
    # Check if method is a simple attr_writer pattern: def x=(v); @x = v; end
    if mn.length <= 1 || mn[mn.length - 1] != "="
      return 0
    end
    if bid < 0 || bid >= @nd_count
      return 0
    end
    # Body should be a StatementsNode with a single InstanceVariableWriteNode
    t = @nd_type[bid]
    if t == "StatementsNode"
      stmts = @nd_stmts[bid]
      if stmts != ""
        parts = stmts.split(",")
        if parts.length == 1
          sid = parts[0].to_i
          if sid >= 0 && sid < @nd_count
            if @nd_type[sid] == "InstanceVariableWriteNode"
              return 1
            end
          end
        end
      end
    end
    # Body might be a single InstanceVariableWriteNode directly
    if t == "InstanceVariableWriteNode"
      return 1
    end
    0
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
    ""
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
      if @needs_str_int_hash == 1 || @needs_str_str_hash == 1
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
    # Multiple passes: value type detection depends on other classes
    2.times do
      i = 0
      while i < @cls_names.length
        names = @cls_ivar_names[i].split(";")
        types = @cls_ivar_types[i].split(";")
        if names.length > 0 && names.length <= 4
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
          if all_val == 1
            @cls_is_value_type[i] = 1
          end
        end
        i = i + 1
      end
    end
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
    # Struct definitions
    i = 0
    while i < @cls_names.length
      emit_raw("struct sp_" + @cls_names[i] + "_s {")
      emit_class_fields(i)
      emit_raw("};")
      emit_raw("")
      i = i + 1
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
              emit_raw("  if (self->" + sanitize_ivar(names[j]) + ") sp_gc_mark(self->" + sanitize_ivar(names[j]) + ");")
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
                  emit_raw("  if (self->" + sanitize_ivar(pnames[pj]) + ") sp_gc_mark(self->" + sanitize_ivar(pnames[pj]) + ");")
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
      emit_raw("static " + c_type(@meth_return_types[i]) + " sp_" + sanitize_name(@meth_names[i]) + "(" + method_params_decl(i) + yp + ");")
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
          emit_raw("static " + c_type(rt) + " sp_" + cname + "_" + sanitize_name(mnames[j]) + "(sp_" + cname + sp + method_with_self_params(j, all_params, all_ptypes) + yp + ");")
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
    cname = @cls_names[ci]
    init_idx = cls_find_method_direct(ci, "initialize")
    if @cls_is_value_type[ci] == 1
      emit_raw("static sp_" + cname + " sp_" + cname + "_new(" + constructor_params_decl(ci) + ") {")
      emit_raw("  sp_" + cname + " self = {0};")
    else
      emit_raw("static inline sp_" + cname + " *sp_" + cname + "_new(" + constructor_params_decl(ci) + ") {")
      emit_raw("  SP_GC_SAVE();")
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
          emit_raw("  self" + sa + pnames2[sk] + " = lv_" + pnames2[sk] + ";")
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
            ivar = sanitize_ivar(@nd_name[sid])
            val = compile_expr(@nd_expression[sid])
            emit_raw("  " + self_arrow + ivar + " = " + val + ";")
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
        stmts = get_stmts(bid)
        stmts.each { |sid|
          if @nd_type[sid] == "InstanceVariableWriteNode"
            ivar = sanitize_ivar(@nd_name[sid])
            val = compile_expr(@nd_expression[sid])
            emit_raw("  " + self_arrow + ivar + " = " + val + ";")
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
    if @cls_is_value_type[ci] == 1
      emit_raw("static " + c_type(rt) + " sp_" + cname + "_" + sanitize_name(mname) + "(sp_" + cname + " self" + build_params_str(pnames, ptypes) + yp + ") {")
    else
      emit_raw("static " + c_type(rt) + " sp_" + cname + "_" + sanitize_name(mname) + "(sp_" + cname + " *self" + build_params_str(pnames, ptypes) + yp + ") {")
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
      emit_raw("static " + c_type(rt) + " sp_" + sanitize_name(mfullname) + "(" + pdecl + ") {")
      push_scope
      declare_var("__self_type", oc_type)
    else
      emit_raw("static " + c_type(@meth_return_types[mi]) + " sp_" + sanitize_name(mfullname) + "(" + method_params_decl(mi) + yp + ") {")
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
      if @needs_gc == 1
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

  def scan_locals(nid, names, types, params)
    if nid < 0
      return
    end
    if @nd_type[nid] == "MultiWriteNode"
      targets = parse_id_list(@nd_targets[nid])
      targets.each { |tid|
        if @nd_type[tid] == "LocalVariableTargetNode"
          lname = @nd_name[tid]
          if not_in(lname, names) == 1
            if not_in(lname, params) == 1
              names.push(lname)
              types.push("int")
            end
          end
        end
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
        end
      else
        if not_in(lname, params) == 1
          # Check if type changed
          at = infer_type(@nd_expression[nid])
          ki = 0
          while ki < names.length
            if names[ki] == lname
              if types[ki] != at
                if types[ki] != "poly"
                  # Don't mark poly if new type is fallback "int" and existing is richer
                  if at != "int"
                    if types[ki] == "int"
                      types[ki] = at
                    elsif at == "nil" && is_nullable_pointer_type(types[ki]) == 1
                      if types[ki][types[ki].length - 1] != "?"
                        types[ki] = types[ki] + "?"
                      end
                    elsif types[ki] == "nil" && is_nullable_pointer_type(at) == 1
                      types[ki] = at + "?"
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
    # Detect object array push: arr.push(ClassName.new(...))
    if @nd_type[nid] == "CallNode"
      if @nd_name[nid] == "push"
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
                @needs_ptr_array = 1
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
              names.push(lname)
              types.push("int")
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
        if bp >= 0
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
                  end
                  mname = @nd_name[nid]
                  if mname == "scan"
                    types.push("string")
                    bk = bk + 1
                    next
                  end
                  if mname == "each"
                    if recv_type == "int_array"
                      if bk == 0
                        types.push("int")
                      else
                        types.push("int")
                      end
                    else
                      if recv_type == "str_array"
                        types.push("string")
                      else
                        if recv_type == "str_int_hash"
                          if bk == 0
                            types.push("string")
                          else
                            types.push("int")
                          end
                        else
                          if recv_type == "str_str_hash"
                            types.push("string")
                          else
                            if recv_type == "poly_array"
                              types.push("poly")
                              @needs_rb_value = 1
                            else
                              types.push("int")
                            end
                          end
                        end
                      end
                    end
                  else
                    if mname == "times"
                      types.push("int")
                    else
                      if mname == "upto"
                        types.push("int")
                      else
                        if mname == "downto"
                          types.push("int")
                        else
                          if mname == "reduce"
                            types.push("int")
                          else
                            if mname == "inject"
                              types.push("int")
                            else
                              if mname == "reject"
                                types.push("int")
                              else
                                types.push("int")
                              end
                            end
                          end
                        end
                      end
                    end
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
    emit_raw("  sp_argv.data=(const char**)(argv+1);sp_argv.len=argc-1;")
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
          if ltypes[k] == "int"
            if ltypes2[j] != "int"
              ltypes[k] = ltypes2[j]
              set_var_type(lnames[k], ltypes2[j])
            end
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
        emit("  " + vol + ctp + "lv_" + lnames[j] + " = NULL;")
        emit("  SP_GC_ROOT(lv_" + lnames[j] + ");")
      else
        emit("  " + vol + ctp + " lv_" + lnames[j] + " = " + c_default_val(ltypes[j]) + ";")
      end
      j = j + 1
    end

    # Constants (initialize global declarations)
    i = 0
    while i < @const_names.length
      val = compile_expr(@const_expr_ids[i])
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

    # Insert lambda and fiber functions before main
    inserted = ""
    if @lambda_funcs != ""
      inserted = inserted + @lambda_funcs
    end
    if @fiber_funcs != ""
      inserted = inserted + @fiber_funcs
    end
    if inserted != ""
      @out = @out[0, @lambda_insert_pos] + inserted + @out[@lambda_insert_pos, @out.length - @lambda_insert_pos]
    end
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
      return c_string_literal(@nd_content[nid])
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
      ci = find_const_idx(@nd_name[nid])
      if ci >= 0
        return "cst_" + @nd_name[nid]
      end
      # Check if inside a module method and constant belongs to that module
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          if @current_method_name.start_with?(mmod + "_cls_")
            cpname = mmod + "_" + @nd_name[nid]
            ci4 = find_const_idx(cpname)
            if ci4 >= 0
              return "cst_" + cpname
            end
          end
          # Also check when in main scope (module constants referenced at top level)
          cpname = mmod + "_" + @nd_name[nid]
          ci5 = find_const_idx(cpname)
          if ci5 >= 0
            return "cst_" + cpname
          end
        end
        mi3 = mi3 + 1
      end
      return @nd_name[nid]
    end
    if t == "ConstantPathNode"
      if @nd_receiver[nid] >= 0
        rname = @nd_name[@nd_receiver[nid]]
        nname = @nd_name[nid]
        cpname = rname + "_" + nname
        ci = find_const_idx(cpname)
        if ci >= 0
          return "cst_" + cpname
        end
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
        return cpname
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
      @needs_file_io = 1
      return "sp_backtick(" + c_string_literal(@nd_content[nid]) + ")"
    end
    if t == "InterpolatedXStringNode"
      @needs_file_io = 1
      @needs_string_helpers = 1
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
    result + "\""
  end

  def compile_interpolated(nid)
    @needs_string_helpers = 1
    parts = parse_id_list(@nd_parts[nid])
    if parts.length == 0
      return "\"\""
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
    cell = heap_promoted_cell(name)
    if cell != ""
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
        if rv >= 0 && @nd_type[rv] == "ConstantReadNode" && @nd_name[rv] == "Fiber"
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
    saved_out = @out
    saved_indent = @indent
    saved_in_fiber_body = @in_fiber_body
    saved_fiber_captures = @fiber_captures
    saved_fiber_capture_types = @fiber_capture_types
    saved_hp_names_len = @heap_promoted_names.length
    saved_hp_cells_len = @heap_promoted_cells.length
    @out = ""
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
        if @nd_type[last] == "LocalVariableWriteNode" || @nd_type[last] == "LocalVariableOperatorWriteNode"
          compile_stmt(last)
          last_val = compile_expr(last)
        else
          last_val = compile_expr(last)
        end
        emit("  _fb->yielded_value = " + box_val_to_poly(last_val, last_type) + ";")
      end
    end
    pop_scope

    fbody = fbody + @out
    fbody = fbody + "}" + 10.chr
    @fiber_funcs = @fiber_funcs + cap_typedef + fbody

    @out = saved_out
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
      if @nd_type[recv] == "ConstantReadNode"
        if @nd_name[recv] == "Fiber"
          return compile_fiber_new(nid)
        end
      end
    end
    # fiber.resume(val)
    if mname == "resume" && recv >= 0
      rt2 = base_type(infer_type(recv))
      if rt2 == "fiber"
        rc = compile_expr(recv)
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length > 0
            return "sp_Fiber_resume(" + rc + ", " + box_expr_to_poly(arg_ids[0]) + ")"
          end
        end
        return "sp_Fiber_resume(" + rc + ", sp_box_nil())"
      end
    end
    # Fiber.yield(val)
    if mname == "yield" && recv >= 0
      if @nd_type[recv] == "ConstantReadNode"
        if @nd_name[recv] == "Fiber"
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
    end
    # fiber.alive?
    if mname == "alive?" && recv >= 0
      rt2 = base_type(infer_type(recv))
      if rt2 == "fiber"
        rc = compile_expr(recv)
        return "sp_Fiber_alive(" + rc + ")"
      end
    end
    # fiber.transfer(val)
    if mname == "transfer" && recv >= 0
      rt2 = base_type(infer_type(recv))
      if rt2 == "fiber"
        rc = compile_expr(recv)
        args_id = @nd_arguments[nid]
        if args_id >= 0
          arg_ids = get_args(args_id)
          if arg_ids.length > 0
            return "sp_Fiber_transfer(" + rc + ", " + box_expr_to_poly(arg_ids[0]) + ")"
          end
        end
        return "sp_Fiber_transfer(" + rc + ", sp_box_nil())"
      end
    end
    # Fiber.current
    if mname == "current" && recv >= 0
      if @nd_type[recv] == "ConstantReadNode"
        if @nd_name[recv] == "Fiber"
          return "sp_fiber_current"
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
    rc = compile_expr(recv)
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

    # String methods
    if recv_type == "string"
      r = compile_string_method_expr(nid, mname, rc)
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
      if @in_yield_method == 1
        return "(_block != NULL)"
      end
      return "0"
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
              # ARGV access || default
              lc = compile_expr(@nd_left[a0])
              rc2 = compile_expr(@nd_right[a0])
              if rt == "int"
                return "((" + lc + ") ? (mrb_int)strtoll(" + lc + ", NULL, 10) : " + rc2 + ")"
              else
                return "((" + lc + ") ? (mrb_int)strtoll(" + lc + ", NULL, 10) : (mrb_int)strtoll(" + rc2 + ", NULL, 10))"
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
        @needs_proc = 1
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
      # p(val) -> puts(val.inspect) - for simplicity, same as puts
      compile_puts(nid)
      return "0"
    end
    if mname == "srand"
      emit("  srand((unsigned)" + compile_arg0(nid) + ");")
      return "0"
    end
    if mname == "rand"
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
      @needs_string_helpers = 1
      return compile_sprintf_call(nid)
    end
    if mname == "sprintf"
      @needs_string_helpers = 1
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
      # Check if function has a &block param and caller provides a block
      ptypes = @meth_param_types[mi].split(",")
      has_block_param = 0
      pk = 0
      while pk < ptypes.length
        if ptypes[pk] == "proc"
          has_block_param = 1
        end
        pk = pk + 1
      end
      if has_block_param == 1
        if @nd_block[nid] >= 0
          @needs_proc = 1
          block_proc = compile_proc_literal(nid)
          return "sp_" + sanitize_name(mname) + "(" + compile_call_args(nid) + ", " + block_proc + ")"
        end
      end
      return "sp_" + sanitize_name(mname) + "(" + compile_call_args_with_defaults(nid, mi) + yargs + ")"
    end
    # Check if we're inside an open class method: implicit self.method
    st = find_var_type("__self_type")
    if st != ""
      # Redirect as self.mname - string methods
      if st == "string"
        @needs_string_helpers = 1
        if mname == "upcase"
          return "sp_str_upcase(self)"
        end
        if mname == "downcase"
          return "sp_str_downcase(self)"
        end
        if mname == "length"
          return "(mrb_int)strlen(self)"
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
          @needs_string_helpers = 1
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
          @needs_string_helpers = 1
          return "sp_float_to_s(self)"
        end
      end
    end
    if @current_class_idx >= 0
      cidx = cls_find_method(@current_class_idx, mname)
      if cidx >= 0
        ca = compile_call_args(nid)
        owner = find_method_owner(@current_class_idx, mname)
        if ca != ""
          return "sp_" + owner + "_" + sanitize_name(mname) + "(self, " + ca + ")"
        else
          return "sp_" + owner + "_" + sanitize_name(mname) + "(self)"
        end
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
    rc = compile_expr(recv)
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
          rc = compile_expr(recv)
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
            rc = compile_expr(recv)
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
          return "sp_proc_call(lv_" + rname + ", " + compile_arg0(nid) + ")"
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
      if mname == "**"
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
    if mname == "**"
      lt = infer_type(recv)
      if lt == "int"
        return "((mrb_int)pow((double)" + compile_expr(recv) + ", (double)" + compile_arg0(nid) + "))"
      end
      return "pow(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
    end
    if mname == "+"
      lt = infer_type(recv)
      if lt == "mutable_str"
        @needs_string_helpers = 1
        return "sp_str_concat(" + compile_expr(recv) + "->data, " + compile_arg0(nid) + ")"
      end
      if lt == "string"
        @needs_string_helpers = 1
        return "sp_str_concat(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
      end
      if lt == "poly"
        @needs_rb_value = 1
        @needs_string_helpers = 1
        return "sp_poly_add(" + compile_expr(recv) + ", " + box_expr_to_poly(@nd_arguments[nid] >= 0 ? get_args(@nd_arguments[nid])[0] : -1) + ")"
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
        @needs_string_helpers = 1
        return "sp_str_repeat(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
      end
      if lt == "poly"
        @needs_rb_value = 1
        return "sp_poly_mul(" + compile_expr(recv) + ", " + box_expr_to_poly(get_args(@nd_arguments[nid])[0]) + ")"
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
      return "sp_imod(" + compile_expr(recv) + ", " + compile_arg0(nid) + ")"
    end
    if mname == "<"
      lt = infer_type(recv)
      if lt == "string"
        return "(strcmp(" + compile_expr(recv) + ", " + compile_arg0(nid) + ") < 0)"
      end
      return "(" + compile_expr(recv) + " < " + compile_arg0(nid) + ")"
    end
    if mname == ">"
      lt = infer_type(recv)
      if lt == "poly"
        @needs_rb_value = 1
        return "sp_poly_gt(" + compile_expr(recv) + ", " + box_expr_to_poly(get_args(@nd_arguments[nid])[0]) + ")"
      end
      return "(" + compile_expr(recv) + " > " + compile_arg0(nid) + ")"
    end
    if mname == "<="
      return "(" + compile_expr(recv) + " <= " + compile_arg0(nid) + ")"
    end
    if mname == ">="
      return "(" + compile_expr(recv) + " >= " + compile_arg0(nid) + ")"
    end
    if mname == "=~"
      # str =~ /pattern/ → sp_re_match(pat, str)
      rc = compile_expr(recv)
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
    if mname == "<<"
      lt = infer_type(recv)
      if lt == "mutable_str"
        @needs_mutable_str = 1
        rc = compile_expr(recv)
        val = compile_arg0(nid)
        return "(sp_String_append(" + rc + ", " + val + "), " + rc + ")"
      end
      if lt == "string"
        @needs_string_helpers = 1
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
    if @nd_type[recv] == "ConstantReadNode"
      cname = @nd_name[recv]
      if cname == "Proc"
        if @nd_block[nid] >= 0
          @needs_proc = 1
          return compile_proc_literal(nid)
        end
      end
      if cname == "Array"
        @needs_gc = 1
        args_id = @nd_arguments[nid]
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
    @needs_string_helpers = 1
    if mname == "length"
      return "(mrb_int)strlen(" + rc + ")"
    end
    if mname == "to_i"
      return "((mrb_int)atoll(" + rc + "))"
    end
    if mname == "to_f"
      return "atof(" + rc + ")"
    end
    if mname == "upcase"
      return "sp_str_upcase(" + rc + ")"
    end
    if mname == "downcase"
      return "sp_str_downcase(" + rc + ")"
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
      return "sp_str_start_with(" + rc + ", " + compile_arg0(nid) + ")"
    end
    if mname == "end_with?"
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
        end
      end
      return "sp_str_split(" + rc + ", " + compile_arg0(nid) + ")"
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
      if args_id >= 0
        a = get_args(args_id)
        if a.length >= 1
          if @nd_type[a[0]] == "RangeNode"
            # s[1..3]
            left = compile_expr(@nd_left[a[0]])
            right = compile_expr(@nd_right[a[0]])
            return "sp_str_sub_range(" + rc + ", " + left + ", " + right + " - " + left + " + 1)"
          end
          if a.length >= 2
            # s[0, 2]
            return "sp_str_sub_range(" + rc + ", " + compile_expr(a[0]) + ", " + compile_expr(a[1]) + ")"
          end
        end
      end
      return "sp_str_sub_range(" + rc + ", " + compile_arg0(nid) + ", 1)"
    end
    if mname == "reverse"
      @needs_string_helpers = 1
      return "sp_str_reverse(" + rc + ")"
    end
    if mname == "freeze"
      return rc
    end
    if mname == "frozen?"
      return "TRUE"
    end
    if mname == "to_sym"
      return rc
    end
    if mname == "ord"
      return "((mrb_int)(unsigned char)" + rc + "[0])"
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
      return "(mrb_int)strlen(" + rc + ")"
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

  def compile_int_method_expr(nid, mname, rc)
    if mname == "to_s"
      @needs_string_helpers = 1
      return "sp_int_to_s(" + rc + ")"
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
      @needs_string_helpers = 1
      return "sp_int_chr(" + rc + ")"
    end
    if mname == "succ"
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
      @needs_string_helpers = 1
      return "sp_float_to_s(" + rc + ")"
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
    ""
  end

  def compile_array_method_expr(nid, mname, rc, recv_type)
    # Array methods
    if recv_type == "int_array"
      if mname == "length"
        return "sp_IntArray_length(" + rc + ")"
      end
      if mname == "[]"
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
      if mname == "sort"
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
        @needs_string_helpers = 1
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
      if mname == "zip"
        # zip returns array of pairs; for length-only usage, return array with same length
        tmp = new_temp
        emit("  sp_IntArray *" + tmp + " = sp_IntArray_new();")
        emit("  for (mrb_int _i = 0; _i < sp_IntArray_length(" + rc + "); _i++) sp_IntArray_push(" + tmp + ", sp_IntArray_get(" + rc + ", _i));")
        return tmp
      end
      if mname == "count"
        if @nd_block[nid] >= 0
          # count with block
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          tmp = new_temp
          itmp = new_temp
          emit("  mrb_int " + tmp + " = 0;")
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
          emit("    if (" + bexpr + ") " + tmp + "++;")
          emit("  }")
          return tmp
        end
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
        return "sp_FloatArray_length(" + rc + ")"
      end
      if mname == "[]"
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
    end
    if is_ptr_array_type(recv_type) == 1
      elem_type = ptr_array_elem_type(recv_type)
      ct = c_type(elem_type)
      if mname == "length" || mname == "size"
        return "sp_PtrArray_length(" + rc + ")"
      end
      if mname == "[]"
        return "(" + ct + ")sp_PtrArray_get(" + rc + ", " + compile_arg0(nid) + ")"
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
        return "sp_StrArray_length(" + rc + ")"
      end
      if mname == "[]"
        return "sp_StrArray_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "first"
        return "sp_StrArray_get(" + rc + ", 0)"
      end
      if mname == "last"
        return "sp_StrArray_get(" + rc + ", sp_StrArray_length(" + rc + ") - 1)"
      end
      if mname == "join"
        @needs_string_helpers = 1
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
      if mname == "count"
        if @nd_block[nid] >= 0
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          declare_var(bp, "string")
          tmp = new_temp
          itmp = new_temp
          emit("  mrb_int " + tmp + " = 0;")
          emit("  for (mrb_int " + itmp + " = 0; " + itmp + " < sp_StrArray_length(" + rc + "); " + itmp + "++) {")
          emit("    const char *lv_" + bp + " = sp_StrArray_get(" + rc + ", " + itmp + ");")
          bbody = @nd_body[blk]
          bexpr = "0"
          if bbody >= 0
            bs = get_stmts(bbody)
            if bs.length > 0
              bexpr = compile_expr(bs.last)
            end
          end
          emit("    if (" + bexpr + ") " + tmp + "++;")
          emit("  }")
          return tmp
        end
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
    if recv_type == "str_int_hash"
      if mname == "[]"
        return "sp_StrIntHash_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?"
        return "sp_StrIntHash_has_key(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "length"
        return "sp_StrIntHash_length(" + rc + ")"
      end
      if mname == "keys"
        return "sp_StrIntHash_keys(" + rc + ")"
      end
      if mname == "merge"
        tmp = new_temp
        arg = compile_arg0(nid)
        emit("  sp_StrIntHash *" + tmp + " = sp_StrIntHash_merge(" + rc + ", " + arg + ");")
        return tmp
      end
      if mname == "transform_values"
        if @nd_block[nid] >= 0
          blk = @nd_block[nid]
          bp = get_block_param(nid, 0)
          declare_var(bp, "int")
          tmp = new_temp
          emit("  sp_StrIntHash *" + tmp + " = sp_StrIntHash_new();")
          emit("  for (mrb_int _i = 0; _i < " + rc + "->len; _i++) {")
          emit("    mrb_int lv_" + bp + " = sp_StrIntHash_get(" + rc + ", " + rc + "->order[_i]);")
          bbody = @nd_body[blk]
          bexpr = "0"
          if bbody >= 0
            bs = get_stmts(bbody)
            if bs.length > 0
              bexpr = compile_expr(bs.last)
            end
          end
          emit("    sp_StrIntHash_set(" + tmp + ", " + rc + "->order[_i], " + bexpr + ");")
          emit("  }")
          return tmp
        end
      end
    end
    if recv_type == "str_str_hash"
      if mname == "[]"
        return "sp_StrStrHash_get(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "has_key?" || mname == "key?"
        return "sp_StrStrHash_has_key(" + rc + ", " + compile_arg0(nid) + ")"
      end
      if mname == "length"
        return "sp_StrStrHash_length(" + rc + ")"
      end
      if mname == "keys"
        return "sp_StrStrHash_keys(" + rc + ")"
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

    # select as expression
    if mname == "select"
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
    # ARGV methods
    if @nd_type[recv] == "ConstantReadNode"
      if @nd_name[recv] == "ARGV"
        if mname == "length"
          return "sp_argv.len"
        end
        if mname == "[]"
          idx_expr = compile_arg0(nid)
          return "((" + idx_expr + " < sp_argv.len) ? sp_argv.data[(int)" + idx_expr + "] : NULL)"
        end
      end
    end

    # Math
    if @nd_type[recv] == "ConstantReadNode"
      rcname = @nd_name[recv]
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
        if mname == "log"
          return "log(" + compile_arg0(nid) + ")"
        end
        if mname == "log2"
          return "log2(" + compile_arg0(nid) + ")"
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
      end
      # File operations
      if rcname == "File"
        if mname == "read"
          @needs_file_io = 1
          return "sp_file_read(" + compile_arg0(nid) + ")"
        end
        if mname == "exist?"
          @needs_file_io = 1
          return "sp_file_exist(" + compile_arg0(nid) + ")"
        end
        if mname == "delete"
          @needs_file_io = 1
          return "(sp_file_delete(" + compile_arg0(nid) + "), 0)"
        end
        if mname == "join"
          @needs_string_helpers = 1
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
          @needs_string_helpers = 1
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
          return "getenv(" + compile_arg0(nid) + ")"
        end
      end
      # Dir
      if rcname == "Dir"
        if mname == "home"
          return "getenv(\"HOME\")"
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
      return "sp_IntArray_from_range(" + compile_expr(@nd_left[range_nid]) + ", " + compile_expr(@nd_right[range_nid]) + ")"
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
            return rc + arrow + mname
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
                return "(" + rc + arrow + bname + " = " + compile_arg0(nid) + ", 0)"
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
          ca = ""
          if midx2 >= 0
            ca = compile_typed_call_args(nid, oci2, midx2)
          else
            ca = compile_call_args(nid)
          end
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
          return "((sp_" + cname2 + " *)" + rc + ")->" + mname
        end
        # Check writers
        if mname.length > 1
          if mname[mname.length - 1] == "="
            bname2 = mname[0, mname.length - 1]
            writers2 = @cls_attr_writers[ci2].split(";")
            j2 = 0
            while j2 < writers2.length
              if writers2[j2] == bname2
                return "(((sp_" + cname2 + " *)" + rc + ")->" + bname2 + " = " + compile_arg0(nid) + ")"
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
            ca2 = compile_typed_call_args(nid, oci3, midx3)
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


  def compile_poly_method_call(nid, rc, mname)
    @needs_rb_value = 1
    if mname == "nil?"
      return "sp_poly_nil_p(" + rc + ")"
    end
    if mname == "to_s"
      return "sp_poly_to_s(" + rc + ")"
    end
    # For object method calls, dispatch based on cls_id
    # Generate: switch on v.tag and cls_id
    tmp = new_temp
    emit("  const char *" + tmp + " = \"\";")
    emit("  if (" + rc + ".tag == SP_TAG_OBJ) {")
    # Dispatch to each possible class
    i = 0
    while i < @cls_names.length
      cname = @cls_names[i]
      # Check if this class has the method
      midx = cls_find_method_direct(i, mname)
      if midx >= 0
        emit("    if (" + rc + ".v.cls_id == " + i.to_s + ") " + tmp + " = sp_" + cname + "_" + sanitize_name(mname) + "((sp_" + cname + " *)" + rc + ".v.p);")
      end
      i = i + 1
    end
    emit("  }")
    tmp
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
    if lt == "string"
      if at == "nil"
        if op == "=="
          return "(" + lc + " == NULL)"
        else
          return "(" + lc + " != NULL)"
        end
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
    if is_obj_type(at) == 1
      cname = at[4, at.length - 4]
      ci = find_class_idx(cname)
      return "sp_box_obj(" + val + ", " + ci.to_s + ")"
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
    "sp_box_int(" + val + ")"
  end

  def compile_call_args_with_defaults(nid, mi)
    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end
    pnames = @meth_param_names[mi].split(",")
    ptypes = @meth_param_types[mi].split(",")
    defaults = @meth_has_defaults[mi].split(",")

    # Check if args contain a KeywordHashNode - extract kw pairs
    kw_names = "".split(",")
    kw_vals = "".split(",")
    positional_ids = []
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
        positional_ids.push(arg_ids[ak])
      end
      ak = ak + 1
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
            # Only splat if there are more positional args than total params
            # (i.e., this is a rest/splat parameter, not a regular array param)
            if positional_ids.length > pnames.length
              @needs_int_array = 1
              @needs_gc = 1
              tmp = new_temp
              emit("  sp_IntArray *" + tmp + " = sp_IntArray_new();")
              pi = 0
              while pi < positional_ids.length
                emit("  sp_IntArray_push(" + tmp + ", " + compile_expr(positional_ids[pi]) + ");")
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
      return compile_call_args(nid)
    end
    # Extract keyword pairs
    kw_names = "".split(",")
    kw_vals = "".split(",")
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
      end
      ak = ak + 1
    end
    # Get init param names from class
    init_ci = find_init_class(ci)
    if init_ci < 0
      return compile_call_args(nid)
    end
    init_idx = cls_find_method_direct(init_ci, "initialize")
    if init_idx < 0
      return compile_call_args(nid)
    end
    all_params = @cls_meth_params[init_ci].split("|")
    pnames = "".split(",")
    if init_idx < all_params.length
      pnames = all_params[init_idx].split(",")
    end
    # Build args in param order using keyword values
    result = ""
    pk = 0
    while pk < pnames.length
      if pk > 0
        result = result + ", "
      end
      found = 0
      ki = 0
      while ki < kw_names.length
        if kw_names[ki] == pnames[pk]
          result = result + kw_vals[ki]
          found = 1
        end
        ki = ki + 1
      end
      if found == 0
        result = result + "0"
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

  def compile_typed_call_args(nid, target_ci, target_midx)
    # Like compile_call_args but casts arguments to match target method param types
    args_id = @nd_arguments[nid]
    if args_id < 0
      return ""
    end
    arg_ids = get_args(args_id)
    all_ptypes = @cls_meth_ptypes[target_ci].split("|")
    ptypes = "".split(",")
    if target_midx < all_ptypes.length
      ptypes = all_ptypes[target_midx].split(",")
    end
    result = ""
    pcname = ""
    k = 0
    while k < arg_ids.length
      if k > 0
        result = result + ", "
      end
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
    cond = compile_expr(@nd_predicate[nid])
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
    cond = compile_expr(@nd_predicate[nid])
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
        else
          if et == "float"
            emit("  sp_PolyArray_push(" + tmp + ", sp_box_float(" + val + "));")
          else
            if et == "bool"
              emit("  sp_PolyArray_push(" + tmp + ", sp_box_bool(" + val + "));")
            else
              if et == "nil"
                emit("  sp_PolyArray_push(" + tmp + ", sp_box_nil());")
              else
                emit("  sp_PolyArray_push(" + tmp + ", sp_box_int(" + val + "));")
              end
            end
          end
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
    if ht == "str_str_hash"
      @needs_str_str_hash = 1
      @needs_string_helpers = 1
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
          emit("  sp_StrStrHash_set(" + tmp + ", " + compile_expr(@nd_key[el]) + ", " + val + ");")
        end
      }
      return tmp
    end
    @needs_str_int_hash = 1
    tmp = new_temp
    emit("  sp_StrIntHash *" + tmp + " = sp_StrIntHash_new();")
    elems.each { |el|
      if @nd_type[el] == "AssocNode"
        emit("  sp_StrIntHash_set(" + tmp + ", " + compile_expr(@nd_key[el]) + ", " + compile_expr(@nd_expression[el]) + ");")
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
      # Empty array literal: create the correct array type
      if vt == "str_array" || vt == "float_array" || is_ptr_array_type(vt) == 1
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
            else
              @needs_ptr_array = 1
              @needs_gc = 1
              emit("  " + vref + " = sp_PtrArray_new();")
            end
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
        emit("  " + vref + " += " + val + ";")
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
      val = compile_expr(@nd_expression[nid])
      # Check if we're in a module class method
      mod_ivar = 0
      mi3 = 0
      while mi3 < @module_names.length
        mmod = @module_names[mi3]
        if mmod != ""
          if @current_method_name.start_with?(mmod + "_cls_")
            iname = @nd_name[nid]
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
        emit("  " + self_arrow + sanitize_ivar(@nd_name[nid]) + " = " + val + ";")
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
    expr = compile_expr(nid)
    if expr != "0"
      emit("  " + expr + ";")
    end
    return
  end

  def compile_multi_write(nid)
    targets = parse_id_list(@nd_targets[nid])
    val_id = @nd_expression[nid]
    if val_id < 0
      return
    end
    if @nd_type[val_id] == "ArrayNode"
      # Direct array literal: a, b, c = [1, 2, 3] or a, b = b, a
      elems = parse_id_list(@nd_elements[val_id])
      # For swap safety, evaluate all RHS first into temps
      tmps = "".split(",")
      k = 0
      while k < elems.length
        tmp = new_temp
        tmps.push(tmp)
        et = infer_type(elems[k])
        emit("  " + c_type(et) + " " + tmp + " = " + compile_expr(elems[k]) + ";")
        k = k + 1
      end
      # Now assign
      k = 0
      while k < targets.length
        if k < tmps.length
          tid = targets[k]
          if @nd_type[tid] == "LocalVariableTargetNode"
            emit("  " + fiber_var_ref(@nd_name[tid]) + " = " + tmps[k] + ";")
          end
          if @nd_type[tid] == "InstanceVariableTargetNode"
            iname = @nd_name[tid]
            # Check if in module method
            mod_ivar = 0
            mi3 = 0
            while mi3 < @module_names.length
              mmod = @module_names[mi3]
              if mmod != ""
                if @current_method_name.start_with?(mmod + "_cls_")
                  cname3 = mmod + "_" + iname[1, iname.length - 1]
                  ci3 = find_const_idx(cname3)
                  if ci3 >= 0
                    emit("  cst_" + cname3 + " = " + tmps[k] + ";")
                    mod_ivar = 1
                  end
                end
              end
              mi3 = mi3 + 1
            end
            if mod_ivar == 0
              emit("  " + self_arrow + sanitize_ivar(iname) + " = " + tmps[k] + ";")
            end
          end
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
    cond = compile_expr(@nd_predicate[nid])
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
    cond = compile_expr(@nd_predicate[nid])
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

  def compile_while_stmt(nid)
    old = @in_loop
    @in_loop = 1
    cond = compile_expr(@nd_predicate[nid])
    emit("  while (" + cond + ") {")
    @indent = @indent + 1
    compile_stmts_body(@nd_body[nid])
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_until_stmt(nid)
    old = @in_loop
    @in_loop = 1
    cond = compile_expr(@nd_predicate[nid])
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
        emit("  for (lv_" + vname + " = " + left + "; lv_" + vname + " <= " + right + "; lv_" + vname + "++) {")
        @indent = @indent + 1
        compile_stmts_body(@nd_body[nid])
        @indent = @indent - 1
        emit("  }")
      else
        # for x in array
        ct = infer_type(coll)
        rc = compile_expr(coll)
        tmp = new_temp
        if ct == "int_array"
          emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
          emit("    lv_" + vname + " = sp_IntArray_get(" + rc + ", " + tmp + ");")
        elsif ct == "str_array"
          emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_StrArray_length(" + rc + "); " + tmp + "++) {")
          emit("    lv_" + vname + " = sp_StrArray_get(" + rc + ", " + tmp + ");")
        elsif ct == "float_array"
          emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_FloatArray_length(" + rc + "); " + tmp + "++) {")
          emit("    lv_" + vname + " = sp_FloatArray_get(" + rc + ", " + tmp + ");")
        else
          emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
          emit("    lv_" + vname + " = sp_IntArray_get(" + rc + ", " + tmp + ");")
        end
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
        result = result + "(" + tmp + " >= " + left + " && " + tmp + " <= " + right + ")"
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
      if arg_ids.length > 0
        if @in_gc_scope == 1
          # Save return value, restore GC, then return
          rt = infer_type(arg_ids[0])
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
    if @in_gc_scope == 1
      emit("  SP_GC_RESTORE();")
    end
    emit("  return 0;")
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
              @needs_file_io = 1
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

    # delete
    if mname == "delete"
      if recv >= 0
        rt = infer_type(recv)
        rc = compile_expr(recv)
        if rt == "str_int_hash"
          emit("  sp_StrIntHash_delete(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
        if rt == "str_str_hash"
          emit("  sp_StrStrHash_delete(" + rc + ", " + compile_arg0(nid) + ");")
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
          rc = compile_expr(recv)
          arg = @nd_arguments[nid]
          if arg >= 0
            argl = parse_id_list(@nd_args[arg])
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
          @needs_string_helpers = 1
          rc = compile_expr(recv)
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

    # replace on string (mutating reassign)
    if mname == "replace"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "mutable_str"
          rc = compile_expr(recv)
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
      end
    end

    # clear on string (set to empty)
    if mname == "clear"
      if recv >= 0
        rt = infer_type(recv)
        if rt == "mutable_str"
          rc = compile_expr(recv)
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
        rc = compile_expr(recv)
        if rt == "int_array"
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
          emit("  sp_StrArray_push(" + rc + ", " + compile_arg0(nid) + ");")
          return 1
        end
      end
    end

    # reverse! / sort!
    if mname == "reverse!"
      if recv >= 0
        rt = infer_type(recv)
        rc = compile_expr(recv)
        if rt == "int_array"
          emit("  sp_IntArray_reverse_bang(" + rc + ");")
          return 1
        end
      end
    end
    if mname == "sort!"
      if recv >= 0
        rt = infer_type(recv)
        rc = compile_expr(recv)
        if rt == "int_array"
          emit("  sp_IntArray_sort_bang(" + rc + ");")
          return 1
        end
      end
    end
    0
  end

  def compile_block_iteration_stmt(nid, mname, recv)
    # each with block
    if mname == "each"
      if @nd_block[nid] >= 0
        # For object types with yield-using each, use yield method call
        if recv >= 0
          ert = infer_type(recv)
          if is_obj_type(ert) == 1
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
                rc = compile_expr(recv)
                bp = get_block_param(nid, 0)
                if bp == ""
                  bp = "_m"
                end
                tmp_arr = new_temp
                tmp_i = new_temp
                emit("  sp_StrArray *" + tmp_arr + " = sp_re_scan(sp_re_pat_" + ridx.to_s + ", " + rc + ");")
                emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < " + tmp_arr + "->len; " + tmp_i + "++) {")
                set_var_type(bp, "string")
                emit("    lv_" + bp + " = " + tmp_arr + "->data[" + tmp_i + "];")
                blk = @nd_block[nid]
                if @nd_body[blk] >= 0
                  compile_stmts_body(@nd_body[blk])
                end
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
        @needs_file_io = 1
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
            @needs_file_io = 1
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
            @needs_file_io = 1
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
    0
  end

  def compile_writer_and_block_call_stmt(nid, mname, recv)
    # attr_writer: obj.x = val
    if recv >= 0
      if mname.length > 1
        if mname[mname.length - 1] == "="
          bname = mname[0, mname.length - 1]
          rt = infer_type(recv)
          if is_obj_type(rt) == 1
            rc = compile_expr(recv)
            arrow2 = "->"
            if is_value_type_obj(rt) == 1
              arrow2 = "."
            end
            emit("  " + rc + arrow2 + bname + " = " + compile_arg0(nid) + ";")
            return 1
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
                declare_var(lbp, "string")
                ltmp = new_temp
                emit("  { char " + ltmp + "[4096];")
                emit("  while (fgets(" + ltmp + ", sizeof(" + ltmp + "), " + ftmp + ")) {")
                emit("    const char *lv_" + lbp + " = " + ltmp + ";")
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
                emit("  } }")
                return
              end
            end
          end
        end
      end
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
        save_out = @out
        save_indent = @indent
        save_hp_names_len = @heap_promoted_names.length
        save_hp_cells_len = @heap_promoted_cells.length
        @out = ""
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
        body_stmts = @out
        @out = save_out
        @indent = save_indent
        # Restore heap promoted
        while @heap_promoted_names.length > save_hp_names_len
          @heap_promoted_names.pop
        end
        while @heap_promoted_cells.length > save_hp_cells_len
          @heap_promoted_cells.pop
        end

        # Build lambda function with typed body
        @lambda_funcs = @lambda_funcs + "static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) {\n"
        if pname != ""
          @lambda_funcs = @lambda_funcs + "  mrb_int lv_" + pname + " = sp_lam_to_int(arg);\n"
        end
        @lambda_funcs = @lambda_funcs + body_stmts + 10.chr
        bexpr = lam_box(last_val, last_type)
        @lambda_funcs = @lambda_funcs + "  return " + bexpr + ";\n"
        @lambda_funcs = @lambda_funcs + "}\n\n"
      elsif bs.length > 0
        # No typed captures: use sp_Val* lambda body compiler
        save_out = @out
        save_params = @lambda_params
        save_captures = @lambda_captures
        save_cell_types = @lambda_capture_cell_types
        @out = ""
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
        body_stmts = @out
        @out = save_out
        @lambda_params = save_params
        @lambda_captures = save_captures
        @lambda_capture_cell_types = save_cell_types

        if body_stmts != ""
          @lambda_funcs = @lambda_funcs + "static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) {\n"
          if pname != ""
            @lambda_funcs = @lambda_funcs + "  sp_Val *lv_" + pname + " = arg;\n"
          end
          @lambda_funcs = @lambda_funcs + "  (void)self;\n"
          @lambda_funcs = @lambda_funcs + body_stmts + 10.chr
          @lambda_funcs = @lambda_funcs + "  return " + bexpr + ";\n"
          @lambda_funcs = @lambda_funcs + "}\n\n"
        else
          @lambda_funcs = @lambda_funcs + "static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) {\n"
          if pname != ""
            @lambda_funcs = @lambda_funcs + "  sp_Val *lv_" + pname + " = arg;\n"
          end
          @lambda_funcs = @lambda_funcs + "  (void)self;\n"
          @lambda_funcs = @lambda_funcs + "  return " + bexpr + ";\n"
          @lambda_funcs = @lambda_funcs + "}\n\n"
        end
      else
        @lambda_funcs = @lambda_funcs + "static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) { (void)self; (void)arg; return &sp_lam_nil_val; }\n\n"
      end
    else
      @lambda_funcs = @lambda_funcs + "static sp_Val *" + fname + "(sp_Val *self, sp_Val *arg) { (void)self; (void)arg; return &sp_lam_nil_val; }\n\n"
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

  def compile_proc_literal(nid)
    blk = @nd_block[nid]
    if blk < 0
      return "sp_proc_new(NULL)"
    end
    bp = get_block_param(nid, 0)
    if bp == ""
      bp = "_unused"
    end
    # Generate a static function for the proc body
    @proc_counter = @proc_counter + 1
    fname = "_sp_proc_fn_" + @proc_counter.to_s
    bbody = @nd_body[blk]
    # Save current output, compile body into a separate buffer
    save_out = @out
    @out = ""
    push_scope
    declare_var(bp, "int")
    bexpr = "0"
    body_stmts = ""
    if bbody >= 0
      bs = get_stmts(bbody)
      if bs.length > 0
        # Compile all statements (including side effects of last)
        k = 0
        while k < bs.length
          lt = infer_type(bs[k])
          if k == bs.length - 1
            if lt != "void"
              # Last statement: compile all previous, then get return expr
              body_stmts = @out
              @out = ""
              bexpr = compile_expr(bs[k])
              extra = @out
              @out = ""
              body_stmts = body_stmts + extra
            else
              # Last is void (like puts): compile as statement, return 0
              compile_stmt(bs[k])
            end
          else
            compile_stmt(bs[k])
          end
          k = k + 1
        end
        if body_stmts == ""
          body_stmts = @out
          @out = ""
        end
      end
    end
    pop_scope
    @out = save_out
    # Build function body
    if body_stmts != ""
      return "({ mrb_int " + fname + "(mrb_int lv_" + bp + ") { " + body_stmts.strip + " return " + bexpr + "; } sp_proc_new(" + fname + "); })"
    end
    return "({ mrb_int " + fname + "(mrb_int lv_" + bp + ") { return " + bexpr + "; } sp_proc_new(" + fname + "); })"
  end

  def compile_bracket_assign(nid)
    recv = @nd_receiver[nid]
    rt = infer_type(recv)
    rc = compile_expr(recv)
    args_id = @nd_arguments[nid]
    arg_ids = []
    if args_id >= 0
      arg_ids = get_args(args_id)
    end
    idx = "0"
    val = "0"
    if arg_ids.length >= 1
      idx = compile_expr(arg_ids[0])
    end
    if arg_ids.length >= 2
      val = compile_expr(arg_ids[1])
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
        emit("  { const char *_ps = " + val + "->data; if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
        k = k + 1
        next
      end
      if at == "bigint"
        emit("  { const char *_bs = sp_bigint_to_s(" + val + "); fputs(_bs, stdout); putchar('" + bsl_n + "'); }")
        k = k + 1
        next
      end
      if at == "int"
        emit("  printf(\"%lld" + bsl_n + "\", (long long)" + val + ");")
      else
        if at == "float"
          emit("  printf(\"%g" + bsl_n + "\", " + val + ");")
        else
          if at == "string" || at == "string?"
            emit("  { const char *_ps = " + val + "; if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
          else
            if at == "bool"
              emit("  puts(" + val + " ? \"true\" : \"false\");")
            else
              if is_obj_type(at) == 1
                cname = at[4, at.length - 4]
                owner = find_method_owner(find_class_idx(cname), "to_s")
                if owner != ""
                  sv = "sp_" + owner + "_to_s(" + (owner == cname ? val : "(sp_" + owner + " *)" + val) + ")"
                  emit("  { const char *_ps = " + sv + "; if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '" + bsl_n + "') putchar('" + bsl_n + "'); } else putchar('" + bsl_n + "'); }")
                else
                  emit("  printf(\"%lld" + bsl_n + "\", (long long)(mrb_int)" + val + ");")
                end
              else
                if at == "str_array"
                  emit("  { sp_StrArray *_pa = " + val + "; for (mrb_int _pi = 0; _pi < _pa->len; _pi++) puts(_pa->data[_pi]); }")
                else
                  if at == "int_array"
                    emit("  { sp_IntArray *_pa = " + val + "; for (mrb_int _pi = 0; _pi < _pa->len; _pi++) printf(\"%lld" + bsl_n + "\", (long long)_pa->data[_pa->start + _pi]); }")
                  else
                    emit("  printf(\"%lld" + bsl_n + "\", (long long)" + val + ");")
                  end
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

  def compile_each_with_index_block(nid)
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    if bp1 == ""
      bp1 = "_v"
    end
    if bp2 == ""
      bp2 = "_idx"
    end
    tmp = new_temp
    if rt == "int_array"
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp + ");")
      emit("    lv_" + bp2 + " = " + tmp + ";")
    elsif rt == "str_array"
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_StrArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp1 + " = sp_StrArray_get(" + rc + ", " + tmp + ");")
      emit("    lv_" + bp2 + " = " + tmp + ";")
    elsif rt == "float_array"
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_FloatArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp1 + " = sp_FloatArray_get(" + rc + ", " + tmp + ");")
      emit("    lv_" + bp2 + " = " + tmp + ";")
    else
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp + ");")
      emit("    lv_" + bp2 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    compile_stmts_body(@nd_body[@nd_block[nid]])
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_each_block(nid)
    old = @in_loop
    @in_loop = 1
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    bp2 = get_block_param(nid, 1)
    has_bp = 1
    if bp1 == ""
      has_bp = 0
      bp1 = "_x"
    end

    if rt == "int_array"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
      if has_bp == 1
        emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp + ");")
      end
      @indent = @indent + 1
      compile_stmts_body(@nd_body[@nd_block[nid]])
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "str_array"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_StrArray_length(" + rc + "); " + tmp + "++) {")
      if has_bp == 1
        emit("    lv_" + bp1 + " = sp_StrArray_get(" + rc + ", " + tmp + ");")
      end
      @indent = @indent + 1
      compile_stmts_body(@nd_body[@nd_block[nid]])
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
      compile_stmts_body(@nd_body[@nd_block[nid]])
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
      compile_stmts_body(@nd_body[@nd_block[nid]])
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "range"
      tmp = new_temp
      tmp2 = new_temp
      emit("  sp_Range " + tmp2 + " = " + rc + ";")
      emit("  for (lv_" + bp1 + " = " + tmp2 + ".first; lv_" + bp1 + " <= " + tmp2 + ".last; lv_" + bp1 + "++) {")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[@nd_block[nid]])
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "poly_array"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_PolyArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp1 + " = sp_PolyArray_get(" + rc + ", " + tmp + ");")
      @indent = @indent + 1
      compile_stmts_body(@nd_body[@nd_block[nid]])
      @indent = @indent - 1
      emit("  }")
    end
    @in_loop = old
  end

  def compile_times_block(nid)
    old = @in_loop
    @in_loop = 1
    rc = compile_expr(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < " + rc + "; " + tmp + "++) {")
    if bp1 != ""
      emit("    lv_" + bp1 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    compile_stmts_body(@nd_body[@nd_block[nid]])
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_upto_block(nid)
    old = @in_loop
    @in_loop = 1
    rc = compile_expr(@nd_receiver[nid])
    lim = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = " + rc + "; " + tmp + " <= " + lim + "; " + tmp + "++) {")
    if bp1 != ""
      emit("    lv_" + bp1 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    compile_stmts_body(@nd_body[@nd_block[nid]])
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_downto_block(nid)
    old = @in_loop
    @in_loop = 1
    rc = compile_expr(@nd_receiver[nid])
    lim = compile_arg0(nid)
    bp1 = get_block_param(nid, 0)
    tmp = new_temp
    emit("  for (mrb_int " + tmp + " = " + rc + "; " + tmp + " >= " + lim + "; " + tmp + "--) {")
    if bp1 != ""
      emit("    lv_" + bp1 + " = " + tmp + ";")
    end
    @indent = @indent + 1
    compile_stmts_body(@nd_body[@nd_block[nid]])
    @indent = @indent - 1
    emit("  }")
    @in_loop = old
  end

  def compile_map_expr(nid)
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
    if rt == "int_array"
      @needs_int_array = 1
      @needs_gc = 1
      # Check if block body returns string (for map that produces StrArray)
      block_ret = "int"
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
      # Check if block param is used as lambda (elements are lambda pointers in IntArray)
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
      if block_ret == "string"
        @needs_str_array = 1
        emit("  sp_StrArray *" + tmp_arr + " = sp_StrArray_new();")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
        if bp_is_lambda == 1
          declare_var(bp1, "lambda")
          emit("    lv_" + bp1 + " = (sp_Val *)sp_IntArray_get(" + rc + ", " + tmp_i + ");")
        else
          emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
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
        return tmp_arr
      else
        emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
        emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
        if bp_is_lambda == 1
          declare_var(bp1, "lambda")
          emit("    lv_" + bp1 + " = (sp_Val *)sp_IntArray_get(" + rc + ", " + tmp_i + ");")
        else
          emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
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
        return tmp_arr
      end
    end
    "0"
  end

  def compile_select_expr(nid)
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array"
      @needs_int_array = 1
      @needs_gc = 1
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
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
    rc = compile_expr(@nd_receiver[nid])
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
    if rt == "int_array"
      tmp = new_temp
      emit("  for (mrb_int " + tmp + " = 0; " + tmp + " < sp_IntArray_length(" + rc + "); " + tmp + "++) {")
      emit("    lv_" + bp2 + " = sp_IntArray_get(" + rc + ", " + tmp + ");")
      @indent = @indent + 1
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
      @indent = @indent - 1
      emit("  }")
    end
  end

  def compile_reject_expr(nid)
    rc = compile_expr(@nd_receiver[nid])
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
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
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
      @indent = @indent - 1
      emit("  }")
      return tmp_arr
    end
    "0"
  end

  def compile_reject_block(nid)
    rc = compile_expr(@nd_receiver[nid])
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
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
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
      @indent = @indent - 1
      emit("  }")
    end
  end

  def compile_sprintf_call(nid)
    @needs_string_helpers = 1
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
    tag = compile_arg0(nid)
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
    tag = compile_arg0(nid)
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
      tag = compile_expr(arg_ids[0])
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
    else
      # No type check - bare rescue, catches all
      sub = @nd_subsequent[rc]
      # Ignore subsequent since bare rescue catches all
    end
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
    rc = compile_expr(recv)

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
    rt = infer_type(@nd_receiver[nid])
    rc = compile_expr(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array"
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
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
      @indent = @indent - 1
      emit("  }")
    end
    if rt == "str_array"
      @needs_str_array = 1
      emit("  sp_StrArray *" + tmp_arr + " = sp_StrArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_StrArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    lv_" + bp1 + " = sp_StrArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
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
    rc = compile_expr(@nd_receiver[nid])
    bp1 = get_block_param(nid, 0)
    if bp1 == ""
      bp1 = "_x"
    end
    tmp_arr = new_temp
    tmp_i = new_temp
    if rt == "int_array"
      emit("  sp_IntArray *" + tmp_arr + " = sp_IntArray_new();")
      emit("  for (mrb_int " + tmp_i + " = 0; " + tmp_i + " < sp_IntArray_length(" + rc + "); " + tmp_i + "++) {")
      emit("    lv_" + bp1 + " = sp_IntArray_get(" + rc + ", " + tmp_i + ");")
      @indent = @indent + 1
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
        if lmname == "proc"
          if return_type != "void"
            @needs_proc = 1
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
      if lm == "[]=" || lm == "push" || lm == "pop" || lm == "emit" || lm == "emit_raw"
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
    cond = compile_expr(@nd_predicate[nid])
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

if out_file != nil
  File.write(out_file, compiler.out)
else
  print compiler.out
end
