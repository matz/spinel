class NodeStore
  attr_reader :count, :root_id

  def initialize
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
    @nd_posts = "".split(",")

    @count = 0
    @root_id = 0

    # Cache for parse_id_list: AST list fields never change once loaded,
    # so the parsed IntArray can be shared across callers.
    @parse_id_cache = {}
    @parse_id_pool = [[0]]
  end

  def node_type(nid)
    @nd_type[nid]
  end

  def node_name(nid)
    @nd_name[nid]
  end

  def set_node_name(nid, val)
    @nd_name[nid] = val
  end

  def node_value(nid)
    @nd_value[nid]
  end

  def node_content(nid)
    @nd_content[nid]
  end

  def node_flags(nid)
    @nd_flags[nid]
  end

  def node_binop(nid)
    @nd_binop[nid]
  end

  def node_unescaped(nid)
    @nd_unescaped[nid]
  end

  def node_receiver(nid)
    @nd_receiver[nid]
  end

  def node_arguments(nid)
    @nd_arguments[nid]
  end

  def node_body(nid)
    @nd_body[nid]
  end

  def node_block(nid)
    @nd_block[nid]
  end

  def set_node_block(nid, val)
    @nd_block[nid] = val
  end

  def node_parameters(nid)
    @nd_parameters[nid]
  end

  def node_predicate(nid)
    @nd_predicate[nid]
  end

  def node_subsequent(nid)
    @nd_subsequent[nid]
  end

  def node_else_clause(nid)
    @nd_else_clause[nid]
  end

  def node_left(nid)
    @nd_left[nid]
  end

  def node_right(nid)
    @nd_right[nid]
  end

  def node_constant_path(nid)
    @nd_constant_path[nid]
  end

  def node_superclass(nid)
    @nd_superclass[nid]
  end

  def node_rest(nid)
    @nd_rest[nid]
  end

  def node_keyword_rest(nid)
    @nd_keyword_rest[nid]
  end

  def node_rescue_clause(nid)
    @nd_rescue_clause[nid]
  end

  def node_ensure_clause(nid)
    @nd_ensure_clause[nid]
  end

  def node_expression(nid)
    @nd_expression[nid]
  end

  def set_node_expression(nid, val)
    @nd_expression[nid] = val
  end

  def node_target(nid)
    @nd_target[nid]
  end

  def node_pattern(nid)
    @nd_pattern[nid]
  end

  def node_key(nid)
    @nd_key[nid]
  end

  def node_reference(nid)
    @nd_reference[nid]
  end

  def node_collection(nid)
    @nd_collection[nid]
  end

  def node_stmts(nid)
    @nd_stmts[nid]
  end

  def node_args(nid)
    @nd_args[nid]
  end

  def node_requireds(nid)
    @nd_requireds[nid]
  end

  def node_optionals(nid)
    @nd_optionals[nid]
  end

  def node_keywords(nid)
    @nd_keywords[nid]
  end

  def node_elements(nid)
    @nd_elements[nid]
  end

  def node_parts(nid)
    @nd_parts[nid]
  end

  def node_conditions(nid)
    @nd_conditions[nid]
  end

  def node_exceptions(nid)
    @nd_exceptions[nid]
  end

  def node_targets(nid)
    @nd_targets[nid]
  end

  def node_rights(nid)
    @nd_rights[nid]
  end

  def node_posts(nid)
    @nd_posts[nid]
  end

  # Parse comma-sep node IDs into IntArray. Manually walks bytes to avoid
  # allocating the intermediate StrArray + substrings that `String#split`
  # would produce. Results are cached by input string: AST fields are
  # immutable once loaded, so the same IntArray can be shared across callers.
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

  def get_stmts(nid)
    if nid < 0
      return []
    end
    if @nd_type[nid] == "StatementsNode"
      return parse_id_list(@nd_stmts[nid])
    end
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

  def has_anonymous_block_forward(nid)
    blk = @nd_block[nid]
    if blk >= 0 && @nd_type[blk] == "BlockArgumentNode" && @nd_expression[blk] < 0
      return 1
    end
    0
  end


  def read_text_ast(data)
    lines = data.split(10.chr)
    max_id = 0
    i = 0
    while i < lines.length
      line = lines[i]
      if line.length > 0
        if line.length >= 6 && line[0] == "R" && line[1] == "O" && line[2] == "O" && line[3] == "T" && line[4] == " "
          @root_id = parse_int_at(line, 5)
        else
          if line.length >= 3 && line[0] == "N" && line[1] == " "
            nid = parse_int_at(line, 2)
            if nid > max_id
              max_id = nid
            end
          end
        end
      end
      i = i + 1
    end

    alloc_node(max_id + 1)

    i = 0
    while i < lines.length
      line = lines[i]
      if line.length > 0
        ast_parse_line(line)
      end
      i = i + 1
    end
  end

  private

  def has_literal_block(nid)
    blk = @nd_block[nid]
    (blk >= 0 && @nd_type[blk] == "BlockNode") ? 1 : 0
  end

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

  def alloc_node(size)
    @nd_type.concat(Array.new(size, ""))
    @nd_name.concat(Array.new(size, ""))
    @nd_value.concat(Array.new(size, 0))
    @nd_content.concat(Array.new(size, ""))
    @nd_flags.concat(Array.new(size, 0))
    @nd_operator.concat(Array.new(size, ""))
    @nd_binop.concat(Array.new(size, ""))
    @nd_callop.concat(Array.new(size, ""))
    @nd_unescaped.concat(Array.new(size, ""))
    @nd_receiver.concat(Array.new(size, -1))
    @nd_arguments.concat(Array.new(size, -1))
    @nd_body.concat(Array.new(size, -1))
    @nd_block.concat(Array.new(size, -1))
    @nd_parameters.concat(Array.new(size, -1))
    @nd_predicate.concat(Array.new(size, -1))
    @nd_subsequent.concat(Array.new(size, -1))
    @nd_else_clause.concat(Array.new(size, -1))
    @nd_left.concat(Array.new(size, -1))
    @nd_right.concat(Array.new(size, -1))
    @nd_constant_path.concat(Array.new(size, -1))
    @nd_superclass.concat(Array.new(size, -1))
    @nd_rest.concat(Array.new(size, -1))
    @nd_keyword_rest.concat(Array.new(size, -1))
    @nd_rescue_clause.concat(Array.new(size, -1))
    @nd_ensure_clause.concat(Array.new(size, -1))
    @nd_expression.concat(Array.new(size, -1))
    @nd_target.concat(Array.new(size, -1))
    @nd_pattern.concat(Array.new(size, -1))
    @nd_key.concat(Array.new(size, -1))
    @nd_reference.concat(Array.new(size, -1))
    @nd_collection.concat(Array.new(size, -1))
    @nd_stmts.concat(Array.new(size, ""))
    @nd_args.concat(Array.new(size, ""))
    @nd_requireds.concat(Array.new(size, ""))
    @nd_optionals.concat(Array.new(size, ""))
    @nd_keywords.concat(Array.new(size, ""))
    @nd_elements.concat(Array.new(size, ""))
    @nd_parts.concat(Array.new(size, ""))
    @nd_conditions.concat(Array.new(size, ""))
    @nd_exceptions.concat(Array.new(size, ""))
    @nd_targets.concat(Array.new(size, ""))
    @nd_rights.concat(Array.new(size, ""))
    @nd_posts.concat(Array.new(size, ""))
    @count = @count + size
  end

  def ast_parse_line(line)
    parts = line.split(" ")
    if parts.length < 3
      return
    end
    tag = parts.first
    nid = parts[1].to_i
    case tag
    in "N"
      @nd_type[nid] = parts[2]
    in "S"
      field = parts[2]
      val = ""
      if parts.length >= 4
        val = unescape_str(parts[3])
      end
      set_string_field(nid, field, val)
    in "I"
      field = parts[2]
      ival = 0
      if parts.length >= 4
        ival = parts[3].to_i
      end
      set_int_field(nid, field, ival)
    in "F"
      if parts.length >= 4
        @nd_content[nid] = parts[3]
      end
    in "R"
      field = parts[2]
      ref_id = -1
      if parts.length >= 4
        ref_id = parts[3].to_i
      end
      set_ref_field(nid, field, ref_id)
    in "A"
      field = parts[2]
      ids_str = ""
      if parts.length >= 4
        ids_str = parts[3]
      end
      set_array_field(nid, field, ids_str)
    else
      nil
    end
    0
  end

  def parse_int_at(s, i)
    num = 0
    while i < s.length
      ch = s[i]
      if ch == " "
        return num
      end
      num = num * 10 + ch.to_i
      i = i + 1
    end
    num
  end

  def unescape_str(s)
    result = ""
    i = 0
    while i < s.length
      ch = s[i]
      if ch == "%"
        if i + 2 < s.length
          hex = s[i + 1] + s[i + 2]
          case hex
          in "0A"
            result << 10.chr
            i = i + 3
          in "0D"
            result << 13.chr
            i = i + 3
          in "09"
            result << 9.chr
            i = i + 3
          in "20"
            result << " "
            i = i + 3
          in "25"
            result << "%"
            i = i + 3
          else
            result << "%" << hex
            i = i + 3
          end
        else
          result << ch
          i = i + 1
        end
      else
        result << ch
        i = i + 1
      end
    end
    result + ""
  end

  def set_string_field(nid, field, val)
    case field
    in "name"
      @nd_name[nid] = val
    in "content"
      @nd_content[nid] = val
    in "value"
      @nd_content[nid] = val
    in "operator"
      @nd_operator[nid] = val
    in "binary_operator"
      @nd_binop[nid] = val
    in "call_operator"
      @nd_callop[nid] = val
    in "unescaped"
      @nd_unescaped[nid] = val
    else
      nil
    end
  end

  def set_int_field(nid, field, val)
    case field
    in "value"
      @nd_value[nid] = val
    in "flags"
      @nd_flags[nid] = val
    in "number"
      @nd_value[nid] = val
    in "maximum"
      @nd_value[nid] = val
    in "start_line"
      @nd_value[nid] = val
    else
      nil
    end
  end

  def set_ref_field(nid, field, ref_id)
    case field
    in "receiver"
      @nd_receiver[nid] = ref_id
    in "arguments"
      @nd_arguments[nid] = ref_id
    in "body"
      @nd_body[nid] = ref_id
    in "block"
      @nd_block[nid] = ref_id
    in "parameters"
      @nd_parameters[nid] = ref_id
    in "predicate"
      @nd_predicate[nid] = ref_id
    in "subsequent"
      @nd_subsequent[nid] = ref_id
    in "else_clause"
      @nd_else_clause[nid] = ref_id
    in "left"
      @nd_left[nid] = ref_id
    in "right"
      @nd_right[nid] = ref_id
    in "constant_path"
      @nd_constant_path[nid] = ref_id
    in "superclass"
      @nd_superclass[nid] = ref_id
    in "rest"
      @nd_rest[nid] = ref_id
    in "keyword_rest"
      @nd_keyword_rest[nid] = ref_id
    in "rescue_clause"
      @nd_rescue_clause[nid] = ref_id
    in "ensure_clause"
      @nd_ensure_clause[nid] = ref_id
    in "expression"
      @nd_expression[nid] = ref_id
    in "target"
      @nd_target[nid] = ref_id
    in "pattern"
      @nd_pattern[nid] = ref_id
    in "key"
      @nd_key[nid] = ref_id
    in "reference"
      @nd_reference[nid] = ref_id
    in "collection"
      @nd_collection[nid] = ref_id
    in "statements"
      @nd_body[nid] = ref_id
    in "value"
      @nd_expression[nid] = ref_id
    in "index"
      @nd_target[nid] = ref_id
    in "parent"
      @nd_receiver[nid] = ref_id
    in "rescue_expression"
      @nd_else_clause[nid] = ref_id
    in "call"
      @nd_receiver[nid] = ref_id
    else
      nil
    end
  end

  def set_array_field(nid, field, ids_str)
    case field
    in "body"
      @nd_stmts[nid] = ids_str
    in "arguments"
      @nd_args[nid] = ids_str
    in "requireds"
      @nd_requireds[nid] = ids_str
    in "optionals"
      @nd_optionals[nid] = ids_str
    in "keywords"
      @nd_keywords[nid] = ids_str
    in "elements"
      @nd_elements[nid] = ids_str
    in "parts"
      @nd_parts[nid] = ids_str
    in "conditions"
      @nd_conditions[nid] = ids_str
    in "exceptions"
      @nd_exceptions[nid] = ids_str
    in "lefts"
      @nd_targets[nid] = ids_str
    in "targets"
      @nd_targets[nid] = ids_str
    in "rights"
      @nd_rights[nid] = ids_str
    in "posts"
      @nd_posts[nid] = ids_str
    else
      nil
    end
  end
end
