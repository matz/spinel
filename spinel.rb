#!/usr/bin/env ruby
# Spinel AOT Compiler (Ruby implementation)
#
# Compiles Ruby source to standalone C via Prism AST and type inference.
# Usage: ruby spinel.rb --source=input.rb --output=output.c

require "prism"
require "stringio"
require "set"

module Spinel
  VERSION = "0.1.0"

  # -------- Type system --------
  module Type
    UNKNOWN   = :unknown
    INTEGER   = :integer
    FLOAT     = :float
    BOOLEAN   = :boolean
    STRING    = :string
    NIL       = :nil
    VOID      = :void
    ARRAY     = :int_array
    STR_ARRAY = :str_array
    HASH      = :str_int_hash
    STR_HASH  = :str_str_hash  # string->string hash (sp_RbHash)
    RANGE     = :range
    MUTABLE_STRING = :mutable_string
    SYMBOL    = :symbol        # treated as string
    TIME      = :time
    FILE_OBJ  = :file_obj
    STRINGIO  = :stringio
    PROC      = :proc
    FLOAT_ARRAY = :float_array # array of mrb_float
    POLY      = :poly       # NaN-boxed sp_RbValue
    POLY_ARRAY = :poly_array # array of sp_RbValue
    POLY_HASH = :poly_hash  # string->sp_RbValue hash
  end

  # -------- Data structures --------
  VarInfo = Struct.new(:name, :type, :c_name, :declared, :is_ivar, :is_constant, :is_global, keyword_init: true)
  MethodInfo = Struct.new(:name, :params, :return_type, :body, :has_yield, :is_class_method,
                          :owner_class, :default_values, :has_rest, :rest_name, :has_kwargs, keyword_init: true)
  ParamInfo = Struct.new(:name, :type, :default_node, keyword_init: true)
  ClassInfo = Struct.new(:name, :parent, :methods, :ivars, :class_methods, :attrs, keyword_init: true)
  BlockEnvEntry = Struct.new(:name, :type, :is_ptr, keyword_init: true)

  # -------- Compiler --------
  class Compiler
    attr_reader :classes, :methods, :constants, :module_constants
    attr_reader :needs_gc, :needs_int_array, :needs_float_array, :needs_str_array, :needs_range
    attr_reader :needs_str_int_hash, :needs_exception, :needs_mutable_string
    attr_reader :needs_str_str_hash, :needs_poly_hash, :needs_block_fn
    attr_reader :string_helpers_needed

    def initialize(source, source_path)
      @source = source
      @source_path = source_path
      @classes = {}
      @methods = {}          # top-level methods: name -> MethodInfo
      @constants = {}        # top-level constants: name -> {type:, value_node:}
      @module_constants = {} # module::CONST -> {type:, c_name:, value:}
      @temp_counter = 0
      @block_counter = 0
      @label_counter = 0
      @needs_gc = false
      @needs_int_array = false
      @needs_float_array = false
      @needs_str_array = false
      @needs_range = false
      @needs_str_int_hash = false
      @needs_str_str_hash = false
      @needs_poly_hash = false
      @needs_exception = false
      @needs_mutable_string = false
      @needs_block_fn = false
      @needs_time = false
      @needs_file = false
      @needs_system = false
      @needs_catch_throw = false
      @needs_stringio = false
      @needs_proc = false
      @needs_poly = false
      @needs_regexp = false
      @regexp_patterns = []  # collected regex patterns: [{pattern:, c_var:}]
      @regexp_pattern_map = {}  # pattern string -> c_var name
      @string_helpers_needed = Set.new
      @forward_decls = []
      @struct_decls = []
      @func_bodies = []
      @block_defs = []       # block struct + function definitions
      @main_vars = []
      @main_body = []
      @indent = 1
      @scope_stack = [{}]    # stack of {varname => VarInfo}
      @current_method = nil
      @current_class = nil
      @in_main = true
      @line_map = {}         # for __LINE__
      @current_retry_label = nil
      @gc_restore_before_return = false  # true when class methods need SP_GC_RESTORE() before return
      @method_refs = {}      # var_name -> method_name for method(:name) tracking
      @array_elem_types = {} # var_name -> class_name for class-typed arrays
      @ivar_elem_types = {}  # class_name -> {ivar_name -> elem_class_name} for typed array ivars
      @ivar_array_sizes = {} # class_name -> {ivar_name -> size} for fixed-size array ivars
      @ivar_hash_value_types = {} # class_name -> {ivar_name -> value_class_name} for class-typed hash values
      @local_hash_value_types = {} # var_name -> value_class_name for local class-typed hash values
    end

    def compile
      # Phase 0: resolve require_relative and merge sources
      @source = resolve_requires(@source, @source_path)

      ast = Prism.parse(@source)
      root = ast.value

      # Phase 1: collect all class/method/constant declarations
      collect_declarations(root)

      # Phase 1.5: scan for mutable string usage
      scan_mutable_strings(root)

      # Phase 1.75: apply module includes (copy module methods into classes)
      apply_module_includes

      # Phase 2: infer types for methods
      infer_method_types

      # Phase 2.25: scan for class-typed arrays (e.g., basis[0] = Vec.new(...))
      scan_class_typed_arrays(root)

      # Phase 2.3: re-infer return types for methods that use class-typed arrays
      @methods.each do |_name, mi|
        next unless mi.body
        rt = infer_body_type(mi.body)
        mi.return_type = rt if rt != Type::UNKNOWN
      end

      # Phase 2.5: detect polymorphic variables and method params
      detect_poly(root)

      # Phase 3: generate code
      generate_code(root)

      # Phase 4: assemble output
      assemble_output
    end

    private

    def resolve_requires(source, source_path)
      base_dir = File.dirname(File.expand_path(source_path))
      resolved = source.dup
      # Process require_relative lines - replace with file contents
      resolved.gsub!(/^require_relative\s+["'](.+?)["']\s*$/) do
        rel_path = $1
        req_file = File.join(base_dir, rel_path)
        req_file += ".rb" unless req_file.end_with?(".rb")
        if File.exist?(req_file)
          # Recursively resolve requires in the included file
          content = File.read(req_file)
          resolve_requires(content, req_file)
        else
          "# require_relative not found: #{rel_path}"
        end
      end
      resolved
    end

    def apply_module_includes
      return unless @module_methods
      @classes.each do |cname, ci|
        includes = ci.attrs[:includes] || []
        includes.each do |mod_name|
          next unless @module_methods[mod_name]
          @module_methods[mod_name].each do |mname, mi|
            next if ci.methods[mname]  # Don't override existing methods
            # Clone the method for this class
            new_mi = MethodInfo.new(
              name: mi.name, params: mi.params.map(&:dup), return_type: mi.return_type,
              body: mi.body, has_yield: mi.has_yield,
              is_class_method: false, owner_class: cname,
              default_values: mi.default_values || {}
            )
            ci.methods[mname] = new_mi
          end
        end
      end
    end

    # ---- Regexp helpers ----
    def register_regexp(pattern)
      @needs_regexp = true
      return @regexp_pattern_map[pattern] if @regexp_pattern_map[pattern]
      idx = @regexp_patterns.length
      c_var = "_re_#{idx}"
      @regexp_patterns << { pattern: pattern, c_var: c_var }
      @regexp_pattern_map[pattern] = c_var
      c_var
    end

    # ---- Temp/label helpers ----
    def next_temp
      t = @temp_counter
      @temp_counter += 1
      t
    end

    def next_block_id
      b = @block_counter
      @block_counter += 1
      b
    end

    def next_label
      l = @label_counter
      @label_counter += 1
      l
    end

    # Sanitize Ruby method names to valid C identifiers
    def sanitize_method_name(name)
      case name
      when "<=>" then "_cmp"
      when "<=" then "_le"
      when ">=" then "_ge"
      when "==" then "_eq"
      when "!=" then "_ne"
      when "<" then "_lt"
      when ">" then "_gt"
      when "+" then "_plus"
      when "-" then "_minus"
      when "*" then "_mul"
      when "/" then "_div"
      when "%" then "_mod"
      when "**" then "_pow"
      when "<<" then "_lshift"
      when ">>" then "_rshift"
      when "&" then "_and"
      when "|" then "_or"
      when "^" then "_xor"
      when "~" then "_not"
      when "[]" then "_aref"
      when "[]=" then "_aset"
      when "-@" then "_uminus"
      when "+@" then "_uplus"
      else
        name.gsub("?", "_p").gsub("!", "_bang").gsub("=", "_set")
      end
    end

    def indent_str
      "  " * @indent
    end

    def emit(line)
      if @in_main
        @main_body << "#{indent_str}#{line}"
      else
        @func_bodies << "#{indent_str}#{line}"
      end
    end

    def emit_raw(line)
      if @in_main
        @main_body << line
      else
        @func_bodies << line
      end
    end

    # Emit a return statement, prepending SP_GC_RESTORE() when inside a
    # GC-managed class method so roots are cleaned up before the frame exits.
    def emit_gc_return(val)
      if @gc_restore_before_return
        emit("SP_GC_RESTORE();")
      end
      emit("return #{val};")
    end

    # ---- Scope management ----
    def push_scope
      @scope_stack.push({})
    end

    def pop_scope
      @scope_stack.pop
    end

    def lookup_var(name)
      @scope_stack.reverse_each do |scope|
        return scope[name] if scope[name]
      end
      nil
    end

    def declare_var(name, type, c_name: nil, is_ivar: false, is_constant: false, is_global: false)
      c_name ||= "lv_#{name}"
      info = VarInfo.new(name: name, type: type, c_name: c_name, declared: false,
                         is_ivar: is_ivar, is_constant: is_constant, is_global: is_global)
      @scope_stack.last[name] = info
      info
    end

    # ---- Phase 1: Collect declarations ----
    def collect_declarations(node)
      return unless node
      case node
      when Prism::ProgramNode
        collect_declarations(node.statements)
      when Prism::StatementsNode
        node.body.each { |s| collect_declarations(s) }
      when Prism::ClassNode
        collect_class(node)
      when Prism::ModuleNode
        collect_module(node)
      when Prism::DefNode
        collect_method(node, nil)
      when Prism::ConstantWriteNode
        collect_constant(node)
      when Prism::CallNode
        # Handle define_method(:name) { ... } as top-level method definition
        if node.name.to_s == "define_method" && node.arguments &&
           node.arguments.arguments.length == 1 &&
           node.arguments.arguments[0].is_a?(Prism::SymbolNode) && node.block
          mname = node.arguments.arguments[0].value
          block = node.block
          params = []
          body = nil
          if block.is_a?(Prism::BlockNode)
            if block.parameters && block.parameters.parameters
              bp = block.parameters.parameters
              (bp.requireds || []).each do |p|
                pname = p.is_a?(Prism::RequiredParameterNode) ? p.name.to_s : p.to_s
                params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: nil)
              end
            end
            body = block.body
          end
          mi = MethodInfo.new(
            name: mname, params: params, return_type: Type::UNKNOWN,
            body: body, has_yield: false,
            is_class_method: false, owner_class: nil,
            default_values: {}
          )
          @methods[mname] = mi
        end
      end
    end

    BUILTIN_TYPES = %w[Integer Float String Boolean Symbol].freeze

    def collect_class(node)
      name = node.constant_path.is_a?(Prism::ConstantReadNode) ? node.constant_path.name.to_s : node.constant_path.to_s
      parent = nil
      is_struct_inherit = false

      # Open class for built-in types: just collect methods, don't create struct
      if BUILTIN_TYPES.include?(name)
        @open_class_methods ||= {}
        @open_class_methods[name] ||= {}
        if node.body
          stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
          stmts.each do |s|
            if s.is_a?(Prism::DefNode) && !s.receiver
              # Instance method on built-in type
              mi_name = s.name.to_s
              params = []
              if s.parameters
                (s.parameters.requireds || []).each do |p|
                  pname = p.is_a?(Prism::RequiredParameterNode) ? p.name.to_s : p.to_s
                  params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: nil)
                end
              end
              mi = MethodInfo.new(
                name: mi_name, params: params, return_type: Type::UNKNOWN,
                body: s.body, has_yield: body_has_yield?(s.body),
                is_class_method: false, owner_class: name,
                default_values: {}
              )
              @open_class_methods[name][mi_name] = mi
            end
          end
        end
        return
      end

      if node.superclass
        if node.superclass.is_a?(Prism::CallNode) && node.superclass.name.to_s == "new" &&
           node.superclass.receiver.is_a?(Prism::ConstantReadNode) && node.superclass.receiver.name.to_s == "Struct"
          # class X < Struct.new(:field1, :field2)
          is_struct_inherit = true
          struct_args = call_args(node.superclass)
          fields = struct_args.select { |a| a.is_a?(Prism::SymbolNode) }.map { |a| a.value }
        else
          parent = case node.superclass
                   when Prism::ConstantReadNode then node.superclass.name.to_s
                   else nil
                   end
        end
      end

      ci = ClassInfo.new(name: name, parent: parent, methods: {}, ivars: {},
                         class_methods: {}, attrs: { reader: [], writer: [], accessor: [] })

      if is_struct_inherit
        fields.each { |f| ci.ivars[f] = Type::INTEGER }
        ci.attrs[:reader] = fields.dup
        ci.attrs[:writer] = fields.dup
        ci.attrs[:accessor] = fields.dup
        # Create synthetic initialize
        params = fields.map { |f| ParamInfo.new(name: f, type: Type::INTEGER) }
        ci.methods["initialize"] = MethodInfo.new(
          name: "initialize", params: params, return_type: Type::VOID,
          body: nil, has_yield: false, is_class_method: false,
          owner_class: name
        )
        @struct_classes ||= {}
        @struct_classes[name] = fields
      end

      @classes[name] = ci

      if node.body
        stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
        stmts.each do |s|
          case s
          when Prism::DefNode
            if s.receiver
              # self.method_name -> class method
              collect_method(s, name, is_class_method: true)
            else
              collect_method(s, name)
            end
          when Prism::CallNode
            collect_attr_call(s, ci)
            # Handle include Comparable / include Enumerable
            if s.name.to_s == "include" && s.arguments
              s.arguments.arguments.each do |arg|
                if arg.is_a?(Prism::ConstantReadNode)
                  mod_name = arg.name.to_s
                  ci.attrs[:includes] ||= []
                  ci.attrs[:includes] << mod_name
                end
              end
            end
          end
        end
      end

      @needs_gc = true if !ci.ivars.empty? || ci.parent
    end

    def collect_module(node)
      mod_name = node.constant_path.is_a?(Prism::ConstantReadNode) ? node.constant_path.name.to_s : node.constant_path.to_s
      @module_methods ||= {}
      @module_methods[mod_name] ||= {}
      @module_class_methods ||= {}
      @module_class_methods[mod_name] ||= {}
      @module_ivars ||= {}
      @module_ivars[mod_name] ||= {}
      if node.body
        stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
        stmts.each do |s|
          if s.is_a?(Prism::ConstantWriteNode)
            cname = s.name.to_s
            type = infer_type(s.value)
            val = const_value_to_c(s.value, type)
            @module_constants["#{mod_name}::#{cname}"] = {
              type: type, c_name: "sp_#{mod_name}_#{cname}", value: val
            }
          elsif s.is_a?(Prism::InstanceVariableWriteNode)
            # Module-level ivar: @x = 123 -> static mrb_int sp_Mod_x = 123;
            ivar = s.name.to_s.delete_prefix("@")
            type = infer_type(s.value)
            @module_ivars[mod_name][ivar] = { type: type, value_node: s.value }
          elsif s.is_a?(Prism::DefNode) && s.receiver
            # Module class method: def self.rand
            mname = s.name.to_s
            params = []
            if s.parameters
              (s.parameters.requireds || []).each do |p|
                pname = p.is_a?(Prism::RequiredParameterNode) ? p.name.to_s : p.to_s
                params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: nil)
              end
            end
            mi = MethodInfo.new(
              name: mname, params: params, return_type: Type::UNKNOWN,
              body: s.body, has_yield: body_has_yield?(s.body),
              is_class_method: true, owner_class: mod_name,
              default_values: {}
            )
            @module_class_methods[mod_name][mname] = mi
          elsif s.is_a?(Prism::DefNode) && !s.receiver
            # Module instance method
            mname = s.name.to_s
            params = []
            if s.parameters
              (s.parameters.requireds || []).each do |p|
                pname = p.is_a?(Prism::RequiredParameterNode) ? p.name.to_s : p.to_s
                params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: nil)
              end
            end
            mi = MethodInfo.new(
              name: mname, params: params, return_type: Type::UNKNOWN,
              body: s.body, has_yield: body_has_yield?(s.body),
              is_class_method: false, owner_class: mod_name,
              default_values: {}
            )
            @module_methods[mod_name][mname] = mi
          end
        end
      end
    end

    def collect_attr_call(node, ci)
      return unless node.is_a?(Prism::CallNode)
      mname = node.name.to_s
      return unless %w[attr_accessor attr_reader attr_writer].include?(mname)

      args = call_args(node)
      args.each do |a|
        sym_name = case a
                   when Prism::SymbolNode then a.value
                   else nil
                   end
        next unless sym_name
        case mname
        when "attr_accessor"
          ci.attrs[:accessor] << sym_name
          ci.attrs[:reader] << sym_name
          ci.attrs[:writer] << sym_name
        when "attr_reader"
          ci.attrs[:reader] << sym_name
        when "attr_writer"
          ci.attrs[:writer] << sym_name
        end
      end
    end

    def collect_method(node, owner, is_class_method: false)
      name = node.name.to_s
      params = []
      defaults = {}
      has_rest = false
      rest_name = nil

      if node.parameters
        # Required params
        (node.parameters.requireds || []).each do |p|
          pname = p.is_a?(Prism::RequiredParameterNode) ? p.name.to_s : p.to_s
          params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: nil)
        end
        # Optional params
        (node.parameters.optionals || []).each do |p|
          pname = p.name.to_s
          params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: p.value)
          defaults[pname] = p.value
        end
        # Rest params (*args)
        if node.parameters.rest.is_a?(Prism::RestParameterNode)
          rest_name = node.parameters.rest.name.to_s
          has_rest = true
          params << ParamInfo.new(name: rest_name, type: Type::ARRAY, default_node: nil)
        end
        # Keyword params (name:, greeting: "Hello")
        (node.parameters.keywords || []).each do |kw|
          pname = kw.name.to_s.chomp(":")
          default_node = kw.respond_to?(:value) ? kw.value : nil
          params << ParamInfo.new(name: pname, type: Type::UNKNOWN, default_node: default_node)
          defaults[pname] = default_node if default_node
        end
      end

      has_yield = body_has_yield?(node.body)
      has_kwargs = node.parameters && (node.parameters.keywords || []).any?

      # Detect &block parameter
      block_param_name = nil
      if node.parameters && node.parameters.block.is_a?(Prism::BlockParameterNode)
        block_param_name = node.parameters.block.name.to_s
        has_yield = true  # &block implies the method receives a block
      end

      mi = MethodInfo.new(
        name: name, params: params, return_type: Type::UNKNOWN,
        body: node.body, has_yield: has_yield,
        is_class_method: is_class_method, owner_class: owner,
        default_values: defaults, has_rest: has_rest, rest_name: rest_name,
        has_kwargs: has_kwargs
      )
      mi.instance_variable_set(:@block_param_name, block_param_name)

      if owner
        if is_class_method
          @classes[owner].class_methods[name] = mi
        else
          @classes[owner].methods[name] = mi
          # Detect simple getter: def x; @x; end -> synthetic attr_reader
          ci = @classes[owner]
          if !name.end_with?("=") && params.empty? && node.body
            body_stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
            if body_stmts.length == 1 && body_stmts[0].is_a?(Prism::InstanceVariableReadNode)
              ivar = body_stmts[0].name.to_s.delete_prefix("@")
              if ivar == name
                ci.attrs[:reader] << name unless ci.attrs[:reader].include?(name)
              end
            end
          end
          # Detect simple setter: def x=(v); @x = v; end -> synthetic attr_writer
          if name.end_with?("=") && params.length == 1 && node.body
            body_stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
            if body_stmts.length == 1 && body_stmts[0].is_a?(Prism::InstanceVariableWriteNode)
              ivar = body_stmts[0].name.to_s.delete_prefix("@")
              field = name.chomp("=")
              if ivar == field
                ci.attrs[:writer] << field unless ci.attrs[:writer].include?(field)
              end
            end
          end
        end
      else
        @methods[name] = mi
      end
    end

    def body_has_yield?(node)
      return false unless node
      case node
      when Prism::YieldNode
        true
      when Prism::StatementsNode
        node.body.any? { |s| body_has_yield?(s) }
      when Prism::IfNode
        body_has_yield?(node.statements) || body_has_yield?(node.subsequent)
      when Prism::WhileNode
        body_has_yield?(node.statements)
      when Prism::CallNode
        # check if block_given? is called (indicates yield usage)
        return true if node.name.to_s == "block_given?"
        body_has_yield?(node.block) if node.block
      when Prism::BlockNode
        body_has_yield?(node.body)
      else
        if node.respond_to?(:statements)
          body_has_yield?(node.statements)
        elsif node.respond_to?(:body)
          body_has_yield?(node.body)
        else
          false
        end
      end
    end

    def collect_constant(node)
      name = node.name.to_s

      # Check for Struct.new(:x, :y)
      if node.value.is_a?(Prism::CallNode) && node.value.name.to_s == "new" &&
         node.value.receiver.is_a?(Prism::ConstantReadNode) && node.value.receiver.name.to_s == "Struct"
        collect_struct_new(name, node.value)
        return
      end

      type = infer_type(node.value)
      @constants[name] = { type: type, node: node.value }
    end

    def collect_struct_new(name, call_node)
      args = call_args(call_node)
      # Filter out keyword_init option
      fields = args.select { |a| a.is_a?(Prism::SymbolNode) }.map { |a| a.value }

      # Create params for constructor
      params = fields.map { |f| ParamInfo.new(name: f, type: Type::INTEGER) }

      ci = ClassInfo.new(name: name, parent: nil, methods: {}, ivars: {},
                         class_methods: {}, attrs: { reader: fields.dup, writer: fields.dup, accessor: fields.dup })
      fields.each { |f| ci.ivars[f] = Type::INTEGER }

      # Create a synthetic initialize method
      ci.methods["initialize"] = MethodInfo.new(
        name: "initialize", params: params, return_type: Type::VOID,
        body: nil, has_yield: false, is_class_method: false,
        owner_class: name
      )

      @classes[name] = ci
      @struct_classes ||= {}
      @struct_classes[name] = fields
    end

    # ---- Phase 2: Type inference ----
    def infer_method_types
      # Auto-generate Comparable methods for classes that include Comparable
      @classes.each do |cname, ci|
        includes = ci.attrs[:includes] || []
        if includes.include?("Comparable") && ci.methods["<=>"]
          # Generate <, >, ==, <=, >= from <=>
          %w[< > == <= >=].each do |op|
            next if ci.methods[op]  # Don't override existing
            is_bool = true
            cmp_op = case op
                     when "<" then "< 0"
                     when ">" then "> 0"
                     when "==" then "== 0"
                     when "<=" then "<= 0"
                     when ">=" then ">= 0"
                     end
            # Store the op and cmp expression for code generation
            ci.methods[op] = MethodInfo.new(
              name: op,
              params: [ParamInfo.new(name: "other", type: Type::UNKNOWN)],
              return_type: Type::BOOLEAN,
              body: nil,
              has_yield: false,
              is_class_method: false,
              owner_class: cname,
              default_values: {}
            )
            # Mark as synthetic for special code generation
            ci.methods[op].instance_variable_set(:@comparable_cmp_op, cmp_op)
          end
        end
      end

      # Pre-collect ivars from initialize bodies
      @classes.each do |_cname, ci|
        init = ci.methods["initialize"]
        collect_ivars_from_body(init.body, ci) if init
      end

      # First pass: scan all call sites to infer param types
      ast = Prism.parse(@source).value
      scan_call_sites(ast)

      # Propagate types from child class constructors to parents via super
      @classes.each do |_cname, ci|
        next unless ci.parent && @classes[ci.parent]
        parent_ci = @classes[ci.parent]
        parent_init = parent_ci.methods["initialize"]
        child_init = ci.methods["initialize"]
        next unless parent_init && child_init

        # Check super calls in child init to propagate param types
        if child_init.body
          propagate_super_types(child_init.body, child_init, parent_init, parent_ci)
        end
      end

      # Update ivar types from param types for all classes
      @classes.each do |_cname, ci|
        init = ci.methods["initialize"]
        next unless init && init.body
        init.params.each do |p|
          next if p.type == Type::UNKNOWN
          ci.ivars.each_key do |iname|
            if ci.ivars[iname] == Type::UNKNOWN
              ci.ivars[iname] = p.type if param_assigned_to_ivar?(init.body, p.name, iname)
            end
          end
        end
      end

      # Infer param types from call sites or defaults
      @methods.each do |_name, mi|
        mi.params.each do |p|
          if p.type == Type::UNKNOWN && p.default_node
            p.type = infer_type(p.default_node)
          end
          if p.type == Type::UNKNOWN
            p.type = Type::INTEGER  # fallback
          end
        end
        mi.return_type = infer_body_type(mi.body)
      end

      # Second pass: re-scan call sites with updated method return types
      # This allows local vars assigned from method calls to get correct types
      ast2 = Prism.parse(@source).value
      scan_call_sites(ast2)

      # Third pass: after var_types_global has been fully updated (including float upgrades),
      # re-scan to propagate upgraded types to method params (e.g., cur_r becomes float
      # from += operator, then mandelbrot(cur_r, cur_i) should get float params)
      ast3 = Prism.parse(@source).value
      scan_call_sites(ast3)

      # Promote INTEGER params to FLOAT when used in float context within method body
      @methods.each do |_name, mi|
        next unless mi.body
        promote_params_from_body(mi)
      end

      # Scan all method bodies for attr assignments to refine nil-initialized ivars
      # e.g., n.left = make_node(x) where n is a Node and left was nil-initialized
      # Done after method return type inference so return types are known
      # Scan all class methods against all classes (cross-class attr setters)
      # Collect method bodies with their owning class for @current_class context
      all_method_entries = []  # [body, owner_class_name_or_nil]
      @classes.each do |cn, ci2|
        ci2.methods.each do |_mn, mi|
          all_method_entries << [mi.body, cn] if mi.body
        end
      end
      @methods.each do |_mn, mi|
        all_method_entries << [mi.body, nil] if mi.body
      end
      # Run multiple passes to resolve self-referential types (e.g., tree node left/right)
      3.times do
        @classes.each do |cname, ci|
          all_method_entries.each do |body, owner_class|
            saved_class = @current_class
            @current_class = owner_class
            scan_ivar_assignments_in_body(body, cname, ci)
            @current_class = saved_class
          end
        end
      end

      # Class methods
      @classes.each do |_cname, ci|
        # Don't re-collect ivars - already done and types updated

        # Also collect from attrs
        (ci.attrs[:accessor] + ci.attrs[:reader] + ci.attrs[:writer]).uniq.each do |attr_name|
          ci.ivars[attr_name] ||= Type::UNKNOWN
        end

        ci.methods.each do |mname, mi|
          mi.params.each do |p|
            if p.default_node
              p.type = infer_type(p.default_node)
            end
          end
          # For setter methods (x=), infer param type from matching ivar
          if mname.end_with?("=") && mi.params.length == 1
            ivar_name = mname.chomp("=")
            if ci.ivars[ivar_name] && ci.ivars[ivar_name] != Type::UNKNOWN &&
               (mi.params[0].type == Type::UNKNOWN || mi.params[0].type == Type::INTEGER)
              mi.params[0].type = ci.ivars[ivar_name]
            end
          end
          # Don't override return type for synthetic methods (e.g., Comparable)
          next if mi.body.nil? && mi.return_type != Type::UNKNOWN
          old_class = @current_class
          @current_class = ci.name
          mi.return_type = infer_body_type(mi.body)
          @current_class = old_class
        end

        # Also infer class method return types
        ci.class_methods.each do |_mname, mi|
          mi.params.each do |p|
            p.type = infer_type(p.default_node) if p.default_node
          end
          old_class = @current_class
          @current_class = ci.name
          mi.return_type = infer_body_type(mi.body)
          @current_class = old_class
        end
      end

      # Infer module class method return types
      if @module_class_methods
        @module_class_methods.each do |mod_name, methods|
          methods.each do |_mname, mi|
            @current_module = mod_name
            mi.return_type = infer_body_type(mi.body)
            @current_module = nil
          end
        end
      end

      # Infer class method param types from body usage
      # e.g., if param b has b.x called and x is a method on Vec, then b is Vec
      @classes.each do |cname, ci|
        ci.methods.each do |_mname, mi|
          next unless mi.body
          mi.params.each do |p|
            next unless p.type == Type::UNKNOWN || p.type == Type::INTEGER
            # Scan body for calls on this param
            called = Set.new
            scan_method_calls_on_param(mi.body, p.name, called)
            next if called.empty?
            # Check which class has all these methods
            @classes.each do |candidate_name, candidate_ci|
              if called.all? { |cm| candidate_ci.methods[cm] ||
                   candidate_ci.attrs[:reader].include?(cm) ||
                   candidate_ci.attrs[:accessor].include?(cm) }
                p.type = candidate_name
                break
              end
            end
          end
        end
      end

      # Also infer class method param types from internal call sites
      # e.g., @center passed to vsub - since @center is Vec, vsub param is Vec
      @classes.each do |cname, ci|
        ci.methods.each do |_mname, mi|
          next unless mi.body
          old_method = @current_method
          @current_method = mi
          infer_class_call_params(mi.body, cname, ci)
          @current_method = old_method
        end
      end
    end

    def detect_poly(root)
      @poly_vars = Set.new       # variable names that need sp_RbValue
      @poly_method_params = {}   # method_name -> Set of param indices that are poly
      @poly_param_classes = {}   # method_name -> { param_idx -> Set of class names }
      @class_tags = {}           # class_name -> tag (0x0040, 0x0041, ...)
      @poly_classes = Set.new    # classes used in poly contexts (need heap alloc)
      @dispatch_methods = {}     # method_name -> Set of class names that implement it

      # Track all types assigned to each variable
      var_types = {}   # var_name -> Set of types
      param_types = {} # method_name -> { param_idx -> Set of types }

      # Scan all assignment sites
      scan_poly_types(root, var_types, param_types)

      # Mark variables with multiple types as poly
      var_types.each do |scoped_vname, types|
        types.delete(Type::UNKNOWN)
        types.delete(Type::NIL)
        if types.size > 1
          # If all types are numeric (INTEGER and FLOAT), coerce to FLOAT, not POLY
          numeric_only = types.all? { |t| t == Type::INTEGER || t == Type::FLOAT }
          unless numeric_only
            # Extract bare variable name from scoped name (scope:name)
            bare_name = scoped_vname.include?(":") ? scoped_vname.split(":", 2).last : scoped_vname
            @poly_vars << bare_name
            @needs_poly = true
          end
        end
        # Also mark if assigned nil AND another type (nilable)
        if types.include?(Type::NIL) && types.any? { |t| t != Type::NIL }
          # Don't need poly for nilable object types
        end
      end

      # Mark method params called with multiple types
      next_tag = 0x0040
      param_types.each do |mname, param_map|
        param_map.each do |idx, types|
          types.delete(Type::UNKNOWN)
          if types.size > 1
            @poly_method_params[mname] ||= Set.new
            @poly_method_params[mname] << idx
            @needs_poly = true
            # Track class types for dispatch
            class_types = types.select { |t| t.is_a?(String) && @classes[t] }
            if class_types.size > 1
              @poly_param_classes[mname] ||= {}
              @poly_param_classes[mname][idx] = class_types.to_set
              class_types.each do |ct|
                unless @class_tags[ct]
                  @class_tags[ct] = next_tag
                  next_tag += 1
                end
                @poly_classes << ct
              end
            end
            # Update the method param type
            if @methods[mname] && idx < @methods[mname].params.length
              @methods[mname].params[idx].type = Type::POLY
            end
          end
        end
      end

      # Detect which methods are called on poly params and build dispatch tables
      @methods.each do |mname, mi|
        next unless mi.body
        mi.params.each_with_index do |p, idx|
          next unless p.type == Type::POLY
          classes = @poly_param_classes.dig(mname, idx)
          next unless classes
          # Scan body for method calls on this param
          called_methods = Set.new
          scan_method_calls_on_param(mi.body, p.name, called_methods)
          called_methods.each do |cm|
            @dispatch_methods[cm] ||= Set.new
            @dispatch_methods[cm].merge(classes)
          end
        end
      end

      # Update method return types if they return poly params or poly expressions
      @methods.each do |mname, mi|
        next unless mi.body
        if mi.params.any? { |p| p.type == Type::POLY }
          if body_returns_poly_param?(mi.body, mi)
            mi.return_type = Type::POLY
          end
          # Re-infer body type considering poly params
          # For methods like show(x) { puts x }, return type should stay as is
        end
      end
    end

    def scan_poly_types(node, var_types, param_types, scope_prefix: "", local_types: nil)
      return unless node
      local_types ||= {}
      case node
      when Prism::ProgramNode
        scan_poly_types(node.statements, var_types, param_types, scope_prefix: scope_prefix, local_types: local_types)
      when Prism::StatementsNode
        node.body.each { |s| scan_poly_types(s, var_types, param_types, scope_prefix: scope_prefix, local_types: local_types) }
      when Prism::DefNode
        # New method scope - use class prefix + method name as scope prefix
        method_scope = node.receiver ? "#{scope_prefix}self.#{node.name}" : "#{scope_prefix}#{node.name}"
        scan_poly_types(node.body, var_types, param_types, scope_prefix: method_scope, local_types: {})
      when Prism::ClassNode
        cname = node.constant_path.is_a?(Prism::ConstantReadNode) ? node.constant_path.name.to_s : ""
        if node.body
          stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
          stmts.each { |s| scan_poly_types(s, var_types, param_types, scope_prefix: "#{cname}#", local_types: {}) }
        end
      when Prism::ModuleNode
        if node.body
          stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
          stmts.each { |s| scan_poly_types(s, var_types, param_types, scope_prefix: scope_prefix, local_types: local_types) }
        end
      when Prism::LocalVariableWriteNode
        # For CallNode values, try local-aware inference first (handles result + sep etc.)
        if node.value.is_a?(Prism::CallNode)
          t = infer_call_type_with_locals(node.value, local_types)
          t = infer_type(node.value) if t == Type::UNKNOWN
        else
          t = infer_type(node.value)
        end
        if t == Type::UNKNOWN && node.value.is_a?(Prism::LocalVariableReadNode)
          t = local_types[node.value.name.to_s] || @var_types_global[node.value.name.to_s] || Type::UNKNOWN
        end
        vname = "#{scope_prefix}:#{node.name}"
        var_types[vname] ||= Set.new
        var_types[vname] << t if t != Type::UNKNOWN
        local_types[node.name.to_s] = t if t != Type::UNKNOWN
        scan_poly_types(node.value, var_types, param_types, scope_prefix: scope_prefix, local_types: local_types)
      when Prism::CallNode
        mname = node.name.to_s
        if @methods[mname] && node.arguments
          mi = @methods[mname]
          node.arguments.arguments.each_with_index do |arg, i|
            next if i >= mi.params.length
            t = infer_type(arg)
            if t == Type::UNKNOWN && arg.is_a?(Prism::LocalVariableReadNode)
              t = @var_types_global[arg.name.to_s] || Type::UNKNOWN
            end
            param_types[mname] ||= {}
            param_types[mname][i] ||= Set.new
            param_types[mname][i] << t if t != Type::UNKNOWN
          end
        end
        node.child_nodes.each { |c| scan_poly_types(c, var_types, param_types, scope_prefix: scope_prefix, local_types: local_types) if c }
      else
        node.child_nodes.each { |c| scan_poly_types(c, var_types, param_types, scope_prefix: scope_prefix, local_types: local_types) if c } if node.respond_to?(:child_nodes)
      end
    end

    # Infer call type using method-local type information for receivers
    def infer_call_type_with_locals(node, local_types)
      return Type::UNKNOWN unless node.is_a?(Prism::CallNode)
      mname = node.name.to_s
      recv_type = if node.receiver.is_a?(Prism::LocalVariableReadNode)
                    local_types[node.receiver.name.to_s] || infer_type(node.receiver)
                  elsif node.receiver.is_a?(Prism::CallNode)
                    # Recursively resolve nested call types (e.g., (result + "%") + hex)
                    infer_call_type_with_locals(node.receiver, local_types)
                  elsif node.receiver
                    infer_type(node.receiver)
                  else
                    nil
                  end

      case mname
      when "+"
        t1 = recv_type || Type::INTEGER
        if (t1 == Type::STRING || t1 == Type::MUTABLE_STRING)
          Type::STRING
        else
          t1
        end
      when "split"
        Type::STR_ARRAY
      when "to_i", "length", "size", "count"
        Type::INTEGER
      when "to_s"
        Type::STRING
      else
        infer_call_type(node)
      end
    end

    # Scan a method body for calls like `param_name.method_name`
    def scan_method_calls_on_param(node, param_name, called_methods)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| scan_method_calls_on_param(s, param_name, called_methods) }
      when Prism::CallNode
        if node.receiver.is_a?(Prism::LocalVariableReadNode) && node.receiver.name.to_s == param_name
          called_methods << node.name.to_s
        end
        node.child_nodes.each { |c| scan_method_calls_on_param(c, param_name, called_methods) if c }
      else
        node.child_nodes.each { |c| scan_method_calls_on_param(c, param_name, called_methods) if c } if node.respond_to?(:child_nodes)
      end
    end

    # Infer param types for class methods from internal call sites
    # e.g., in Sphere.intersect, ray.dir calls are on Ray type, isect.t calls on Isect type
    def infer_class_call_params(node, cname, ci)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| infer_class_call_params(s, cname, ci) }
      when Prism::CallNode
        mname = node.name.to_s
        # Check method calls on local vars: var.method(args)
        if node.receiver.is_a?(Prism::LocalVariableReadNode) && node.arguments
          recv_name = node.receiver.name.to_s
          # Get receiver type from var_types_global or scope
          recv_class = @var_types_global[recv_name]
          if recv_class.is_a?(String) && @classes[recv_class]
            rci = @classes[recv_class]
            rmi = rci.methods[mname]
            if rmi
              node.arguments.arguments.each_with_index do |arg, i|
                next if i >= rmi.params.length
                arg_type = infer_type(arg)
                if arg_type == Type::UNKNOWN
                  # Check if arg is an ivar read
                  if arg.is_a?(Prism::InstanceVariableReadNode)
                    iname = arg.name.to_s.delete_prefix("@")
                    arg_type = ci.ivars[iname] if ci.ivars[iname]
                  elsif arg.is_a?(Prism::LocalVariableReadNode)
                    arg_type = @var_types_global[arg.name.to_s] || Type::UNKNOWN
                  end
                end
                if arg_type != Type::UNKNOWN && (rmi.params[i].type == Type::UNKNOWN ||
                   (arg_type == Type::FLOAT && rmi.params[i].type == Type::INTEGER) ||
                   (arg_type.is_a?(String) && @classes[arg_type] && rmi.params[i].type == Type::INTEGER))
                  rmi.params[i].type = arg_type
                end
              end
            end
          end
        end
        # Check no-receiver calls (implicit self or top-level): method(args)
        if node.receiver.nil? && !mname.end_with?("=") && node.arguments
          target_mi = ci.methods[mname] || @methods[mname]
          if target_mi
            node.arguments.arguments.each_with_index do |arg, i|
              next if i >= target_mi.params.length
              arg_type = infer_type(arg)
              if arg_type == Type::UNKNOWN
                if arg.is_a?(Prism::InstanceVariableReadNode)
                  iname = arg.name.to_s.delete_prefix("@")
                  arg_type = ci.ivars[iname] if ci.ivars[iname]
                elsif arg.is_a?(Prism::LocalVariableReadNode)
                  arg_type = @var_types_global[arg.name.to_s] || Type::UNKNOWN
                elsif arg.is_a?(Prism::CallNode)
                  # For chained calls like isect.n, infer the return type
                  arg_type = infer_call_return_type_in_class(arg, ci)
                end
              end
              if arg_type != Type::UNKNOWN && (target_mi.params[i].type == Type::UNKNOWN ||
                 (arg_type == Type::FLOAT && target_mi.params[i].type == Type::INTEGER) ||
                 (arg_type.is_a?(String) && @classes[arg_type] && target_mi.params[i].type == Type::INTEGER))
                target_mi.params[i].type = arg_type
              end
            end
          end
        end
        node.child_nodes.each { |c| infer_class_call_params(c, cname, ci) if c }
      else
        node.child_nodes.each { |c| infer_class_call_params(c, cname, ci) if c } if node.respond_to?(:child_nodes)
      end
    end

    # Infer the return type of a call expression within a class context
    # Handles chained calls like isect.n where isect is a class method param
    def infer_call_return_type_in_class(node, ci)
      return Type::UNKNOWN unless node.is_a?(Prism::CallNode)
      mname = node.name.to_s
      if node.receiver.is_a?(Prism::LocalVariableReadNode)
        recv_name = node.receiver.name.to_s
        # Check if receiver is a method param with a class type
        if @current_method
          param = @current_method.params.find { |p| p.name == recv_name }
          if param && param.type.is_a?(String) && @classes[param.type]
            rci = @classes[param.type]
            rmi = rci.methods[mname]
            return rmi.return_type if rmi && rmi.return_type != Type::UNKNOWN
            return rci.ivars[mname] if rci.ivars[mname] && rci.ivars[mname] != Type::UNKNOWN
          end
        end
      end
      Type::UNKNOWN
    end

    def body_returns_poly_param?(body, mi)
      return false unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      last = stmts.last
      return false unless last
      expr_involves_poly?(last, mi)
    end

    def expr_involves_poly?(node, mi)
      return false unless node
      case node
      when Prism::LocalVariableReadNode
        mi.params.any? { |p| p.name == node.name.to_s && p.type == Type::POLY }
      when Prism::CallNode
        mname = node.name.to_s
        # Comparison operators return boolean, not poly
        return false if %w[== != < > <= >= <=> !].include?(mname)
        # to_s returns string, not poly
        return false if mname == "to_s"
        # Check if any operand involves a poly param
        recv_poly = node.receiver ? expr_involves_poly?(node.receiver, mi) : false
        args_poly = node.arguments&.arguments&.any? { |a| expr_involves_poly?(a, mi) } || false
        recv_poly || args_poly
      else
        false
      end
    end

    def scan_mutable_strings(node)
      @mutable_string_vars = Set.new
      find_mutable_strings(node)
    end

    def find_mutable_strings(node)
      return unless node
      case node
      when Prism::ProgramNode
        find_mutable_strings(node.statements)
      when Prism::StatementsNode
        node.body.each { |s| find_mutable_strings(s) }
      when Prism::CallNode
        mname = node.name.to_s
        if (mname == "<<" || mname == "replace" || mname == "clear" || mname == "gsub!" || mname == "sub!" ||
            mname == "upcase!" || mname == "downcase!" || mname == "strip!" || mname == "chomp!" ||
            mname == "setbyte") &&
           node.receiver.is_a?(Prism::LocalVariableReadNode)
          recv_type = @var_types_global && @var_types_global[node.receiver.name.to_s]
          if recv_type == Type::STRING || recv_type.nil?
            @mutable_string_vars << node.receiver.name.to_s
          end
        end
        node.child_nodes.each { |c| find_mutable_strings(c) if c }
      when Prism::DefNode
        find_mutable_strings(node.body)
      else
        node.child_nodes.each { |c| find_mutable_strings(c) if c } if node.respond_to?(:child_nodes)
      end
    end

    # Scan for arrays that hold class instances (e.g., basis[0] = Vec.new(...), @spheres[0] = Sphere.new(...))
    def scan_class_typed_arrays(root)
      @array_elem_types = {}
      @ivar_elem_types = {}
      @ivar_array_sizes = {}
      @local_array_sizes = {} # var_name -> size for Array.new(N)
      # Scan all class method bodies
      @classes.each do |cname, ci|
        ci.methods.each do |_mname, mi|
          next unless mi.body
          scan_array_elem_assigns(mi.body, cname)
        end
      end
      # Scan top-level method bodies
      @methods.each do |_mname, mi|
        next unless mi.body
        scan_array_elem_assigns(mi.body, nil)
      end
      # Scan main body
      scan_array_elem_assigns(root, nil)

      # Fix up method parameter types for class-typed arrays
      # When a method is called with a class-typed array arg, update the param type
      fix_method_params_for_typed_arrays(root)
    end

    def fix_method_params_for_typed_arrays(node)
      return unless node
      case node
      when Prism::ProgramNode
        fix_method_params_for_typed_arrays(node.statements)
      when Prism::StatementsNode
        node.body.each { |s| fix_method_params_for_typed_arrays(s) }
      when Prism::CallNode
        mname = node.name.to_s
        # Check if this is a call to a known top-level method or class method
        mi = @methods[mname]
        if mi && node.arguments
          node.arguments.arguments.each_with_index do |arg, i|
            next if i >= mi.params.length
            if arg.is_a?(Prism::LocalVariableReadNode)
              elem_class = @array_elem_types[arg.name.to_s]
              if elem_class && mi.params[i].type == Type::ARRAY
                mi.params[i].type = elem_class
                mi.params[i].instance_variable_set(:@is_typed_array, true)
              end
            end
          end
        end
        # Also check implicit self calls in class methods
        if !node.receiver && @current_class
          ci = @classes[@current_class]
          actual = find_method_class(@current_class, mname) if ci
          if actual && @classes[actual].methods[mname] && node.arguments
            ami = @classes[actual].methods[mname]
            node.arguments.arguments.each_with_index do |arg, i|
              next if i >= ami.params.length
              if arg.is_a?(Prism::LocalVariableReadNode)
                elem_class = @array_elem_types[arg.name.to_s]
                if elem_class && ami.params[i].type == Type::ARRAY
                  ami.params[i].type = elem_class
                  ami.params[i].instance_variable_set(:@is_typed_array, true)
                end
              end
            end
          end
        end
        node.child_nodes.each { |c| fix_method_params_for_typed_arrays(c) if c }
      when Prism::DefNode
        old_class = @current_class
        fix_method_params_for_typed_arrays(node.body)
        @current_class = old_class
      when Prism::ClassNode
        old_class = @current_class
        cname = node.constant_path.is_a?(Prism::ConstantReadNode) ? node.constant_path.name.to_s : nil
        @current_class = cname if cname
        if node.body
          stmts = node.body.is_a?(Prism::StatementsNode) ? node.body.body : [node.body]
          stmts.each { |s| fix_method_params_for_typed_arrays(s) }
        end
        @current_class = old_class
      else
        node.child_nodes.each { |c| fix_method_params_for_typed_arrays(c) if c } if node.respond_to?(:child_nodes)
      end
    end

    def scan_array_elem_assigns(node, context_class)
      return unless node
      case node
      when Prism::ProgramNode
        scan_array_elem_assigns(node.statements, context_class)
      when Prism::StatementsNode
        node.body.each { |s| scan_array_elem_assigns(s, context_class) }
      when Prism::LocalVariableWriteNode
        # Detect var = Array.new(N) patterns
        val = node.value
        if val.is_a?(Prism::CallNode) && val.name.to_s == "new" &&
           val.receiver.is_a?(Prism::ConstantReadNode) && val.receiver.name.to_s == "Array" &&
           val.arguments && val.arguments.arguments.length == 1 &&
           val.arguments.arguments[0].is_a?(Prism::IntegerNode)
          @local_array_sizes[node.name.to_s] = val.arguments.arguments[0].value
        end
        scan_array_elem_assigns(val, context_class)
      when Prism::CallNode
        mname = node.name.to_s
        if mname == "[]=" && node.arguments && node.arguments.arguments.length == 2
          val_node = node.arguments.arguments[1]
          elem_class = nil
          # Check if value is ClassName.new(...)
          if val_node.is_a?(Prism::CallNode) && val_node.name.to_s == "new" &&
             val_node.receiver.is_a?(Prism::ConstantReadNode)
            elem_class = val_node.receiver.name.to_s
            elem_class = nil unless @classes[elem_class]
          end
          # Check if value is a local variable with known class type
          if elem_class.nil? && val_node.is_a?(Prism::LocalVariableReadNode)
            vt = infer_type(val_node)
            if vt.is_a?(String) && @classes[vt]
              elem_class = vt
            end
          end
          # Check if value is a function call returning a class type
          if elem_class.nil? && val_node.is_a?(Prism::CallNode)
            vt = infer_call_type(val_node)
            if vt.is_a?(String) && @classes[vt]
              elem_class = vt
            end
          end
          if elem_class
            recv = node.receiver
            idx_node = node.arguments.arguments[0]
            is_hash_key = idx_node.is_a?(Prism::StringNode) || idx_node.is_a?(Prism::SymbolNode) ||
                          idx_node.is_a?(Prism::LocalVariableReadNode)
            if recv.is_a?(Prism::InstanceVariableReadNode) && context_class
              ivar_name = recv.name.to_s.delete_prefix("@")
              if is_hash_key && !idx_node.is_a?(Prism::IntegerNode)
                # Hash with class-typed values: @hash[key] = ClassName.new(...)
                @ivar_hash_value_types[context_class] ||= {}
                @ivar_hash_value_types[context_class][ivar_name] ||= elem_class
              else
                @ivar_elem_types[context_class] ||= {}
                @ivar_elem_types[context_class][ivar_name] ||= elem_class
                # Detect array size from index
                if idx_node.is_a?(Prism::IntegerNode)
                  @ivar_array_sizes[context_class] ||= {}
                  cur = @ivar_array_sizes[context_class][ivar_name] || 0
                  @ivar_array_sizes[context_class][ivar_name] = [cur, idx_node.value + 1].max
                end
              end
            elsif recv.is_a?(Prism::LocalVariableReadNode)
              var_name = recv.name.to_s
              if is_hash_key && !idx_node.is_a?(Prism::IntegerNode)
                @local_hash_value_types[var_name] ||= elem_class
              else
                @array_elem_types[var_name] ||= elem_class
              end
            end
          end
        end
        # Also detect hash[key] = value via method calls (MethodInfo stored in hash)
        if mname == "push" && node.arguments && node.arguments.arguments.length == 1
          val_node = node.arguments.arguments[0]
          if val_node.is_a?(Prism::LocalVariableReadNode)
            val_type = lookup_var(val_node.name.to_s)
            if val_type && val_type.type.is_a?(String) && @classes[val_type.type]
              recv = node.receiver
              if recv.is_a?(Prism::InstanceVariableReadNode) && context_class
                ivar_name = recv.name.to_s.delete_prefix("@")
                @ivar_elem_types[context_class] ||= {}
                @ivar_elem_types[context_class][ivar_name] ||= val_type.type
              elsif recv.is_a?(Prism::LocalVariableReadNode)
                @array_elem_types[recv.name.to_s] ||= val_type.type
              end
            end
          end
        end
        # Recurse into all child nodes
        node.child_nodes.each { |c| scan_array_elem_assigns(c, context_class) if c }
      when Prism::DefNode
        scan_array_elem_assigns(node.body, context_class)
      when Prism::ClassNode
        # Don't recurse into nested class definitions
      when Prism::ModuleNode
        # Don't recurse into modules
      else
        node.child_nodes.each { |c| scan_array_elem_assigns(c, context_class) if c } if node.respond_to?(:child_nodes)
      end
    end

    def scan_call_sites(node)
      @var_types_global ||= {}
      return unless node
      case node
      when Prism::ProgramNode
        scan_call_sites(node.statements)
      when Prism::StatementsNode
        node.body.each { |s| scan_call_sites(s) }
      when Prism::LocalVariableWriteNode
        t = infer_type(node.value)
        # Fallback: if value is a [] call on a local whose type is in var_types_global,
        # use the element type (e.g., StrArray#[] -> STRING)
        if t == Type::UNKNOWN && node.value.is_a?(Prism::CallNode) && node.value.name.to_s == "[]" &&
           node.value.receiver.is_a?(Prism::LocalVariableReadNode)
          recv_gt = @var_types_global[node.value.receiver.name.to_s]
          case recv_gt
          when Type::STR_ARRAY then t = Type::STRING
          when Type::ARRAY then t = Type::INTEGER
          when Type::FLOAT_ARRAY then t = Type::FLOAT
          when Type::HASH then t = Type::INTEGER
          when Type::STR_HASH then t = Type::STRING
          end
        end
        @var_types_global[node.name.to_s] = t if t != Type::UNKNOWN
        scan_call_sites(node.value)
      when Prism::LocalVariableOperatorWriteNode
        val_type = infer_type(node.value)
        # If value is a local var reference, also check var_types_global and method params
        if val_type == Type::UNKNOWN && node.value.is_a?(Prism::LocalVariableReadNode)
          ref = node.value.name.to_s
          val_type = @var_types_global[ref] if @var_types_global[ref]
          # Check all method params if still unknown
          if val_type == Type::UNKNOWN || val_type.nil?
            @methods.each_value do |mi|
              p = mi.params.find { |pp| pp.name == ref }
              if p && p.type != Type::UNKNOWN
                val_type = p.type
                break
              end
            end
          end
        end
        vname = node.name.to_s
        existing = @var_types_global[vname]
        # Upgrade to float if operator involves float (e.g., x += 0.5)
        if val_type == Type::FLOAT && (existing.nil? || existing == Type::INTEGER)
          @var_types_global[vname] = Type::FLOAT
        end
      when Prism::CallNode
        mname = node.name.to_s
        if @methods[mname] && node.arguments
          mi = @methods[mname]
          node.arguments.arguments.each_with_index do |arg, i|
            # Handle keyword hash nodes - extract types by name
            if arg.is_a?(Prism::KeywordHashNode)
              arg.elements.each do |assoc|
                next unless assoc.is_a?(Prism::AssocNode) && assoc.key.is_a?(Prism::SymbolNode)
                key_name = assoc.key.value
                arg_type = infer_type(assoc.value)
                if arg_type != Type::UNKNOWN
                  mi.params.each do |p|
                    p.type = arg_type if p.name == key_name && p.type == Type::UNKNOWN
                  end
                end
              end
              next
            end
            next if i >= mi.params.length
            arg_type = infer_arg_type_for_scan(arg)
            can_upgrade = mi.params[i].type == Type::UNKNOWN ||
              (mi.params[i].type == Type::INTEGER && arg_type != Type::INTEGER && arg_type != Type::BOOLEAN && arg_type != Type::NIL)
            if arg_type != Type::UNKNOWN && can_upgrade
              mi.params[i].type = arg_type
            end
          end
        end
        # Scan class constructor calls: ClassName.new(args)
        if mname == "new" && node.receiver.is_a?(Prism::ConstantReadNode) && node.arguments
          cname = node.receiver.name.to_s
          if @classes[cname]
            ci = @classes[cname]
            init = ci.methods["initialize"]
            # Inherit parent initialize if not defined
            if init.nil? && ci.parent && @classes[ci.parent]
              init = @classes[ci.parent].methods["initialize"]
              ci = @classes[ci.parent]  # use parent for ivar updates
            end
            if init
              # Handle keyword arguments for struct constructors
              if node.arguments.arguments.length == 1 &&
                 node.arguments.arguments[0].is_a?(Prism::KeywordHashNode) &&
                 @struct_classes && @struct_classes[cname]
                kw_node = node.arguments.arguments[0]
                kw_node.elements.each do |assoc|
                  next unless assoc.is_a?(Prism::AssocNode) && assoc.key.is_a?(Prism::SymbolNode)
                  field_name = assoc.key.value
                  arg_type = infer_type(assoc.value)
                  if arg_type != Type::UNKNOWN
                    ci.ivars[field_name] = arg_type if ci.ivars[field_name] == Type::INTEGER || ci.ivars[field_name] == Type::UNKNOWN
                    # Also update param type
                    init.params.each do |p|
                      p.type = arg_type if p.name == field_name && (p.type == Type::UNKNOWN || p.type == Type::INTEGER)
                    end
                  end
                end
              else
                node.arguments.arguments.each_with_index do |arg, i|
                  next if i >= init.params.length
                  arg_type = infer_type(arg)
                  if arg_type == Type::UNKNOWN && arg.is_a?(Prism::LocalVariableReadNode)
                    arg_type = @var_types_global[arg.name.to_s] || Type::UNKNOWN
                  end
                  if arg_type != Type::UNKNOWN
                    if init.params[i].type == Type::UNKNOWN ||
                       (@struct_classes && @struct_classes[cname]) ||
                       (arg_type == Type::FLOAT && init.params[i].type == Type::INTEGER) ||
                       (arg_type.is_a?(String) && @classes[arg_type] && init.params[i].type == Type::INTEGER)
                      init.params[i].type = arg_type
                    end
                    # Update ivar types
                    pname = init.params[i].name
                    if @struct_classes && @struct_classes[cname]
                      # For struct classes, fields map directly to params
                      ci.ivars[pname] = arg_type if ci.ivars[pname]
                    else
                      ci.ivars.each_key do |iname|
                        if ci.ivars[iname] == Type::UNKNOWN ||
                           (arg_type == Type::FLOAT && ci.ivars[iname] == Type::INTEGER) ||
                           (arg_type.is_a?(String) && @classes[arg_type] && ci.ivars[iname] == Type::INTEGER)
                          ci.ivars[iname] = arg_type if param_assigned_to_ivar?(init.body, pname, iname)
                        end
                      end
                    end
                  end
                end
              end
            end
          end
        end
        # Scan attr writer calls: obj.attr = value (name ends with =)
        if mname.end_with?("=") && node.receiver.is_a?(Prism::LocalVariableReadNode) && node.arguments
          attr_name = mname.chomp("=")
          recv_name = node.receiver.name.to_s
          recv_class = @var_types_global[recv_name]
          if recv_class.is_a?(String) && @classes[recv_class]
            ci = @classes[recv_class]
            if (ci.attrs[:writer].include?(attr_name) || ci.attrs[:accessor].include?(attr_name))
              node.arguments.arguments.each do |arg|
                arg_type = infer_type(arg)
                if arg_type == Type::UNKNOWN && arg.is_a?(Prism::LocalVariableReadNode)
                  arg_type = @var_types_global[arg.name.to_s] || Type::UNKNOWN
                end
                if arg_type != Type::UNKNOWN && (ci.ivars[attr_name] == Type::UNKNOWN || ci.ivars[attr_name] == Type::NIL)
                  ci.ivars[attr_name] = arg_type
                end
              end
            end
          end
        end
        # Scan instance method calls: obj.method(args) to infer param types
        if node.receiver.is_a?(Prism::LocalVariableReadNode) && !mname.end_with?("=") && mname != "new" && node.arguments
          recv_name = node.receiver.name.to_s
          recv_class = @var_types_global[recv_name]
          if recv_class.is_a?(String) && @classes[recv_class]
            ci = @classes[recv_class]
            mi = ci.methods[mname]
            if mi
              node.arguments.arguments.each_with_index do |arg, i|
                next if i >= mi.params.length
                arg_type = infer_type(arg)
                if arg_type == Type::UNKNOWN && arg.is_a?(Prism::LocalVariableReadNode)
                  arg_type = @var_types_global[arg.name.to_s] || Type::UNKNOWN
                end
                if arg_type != Type::UNKNOWN && mi.params[i].type == Type::UNKNOWN
                  mi.params[i].type = arg_type
                end
              end
            end
          end
        end
        # Scan implicit self calls (no receiver) to class methods
        if node.receiver.nil? && node.arguments
          @classes.each do |_cname, ci|
            mi = ci.methods[mname]
            if mi
              node.arguments.arguments.each_with_index do |arg, i|
                next if i >= mi.params.length
                arg_type = infer_arg_type_for_scan(arg)
                if arg_type != Type::UNKNOWN && mi.params[i].type == Type::UNKNOWN
                  mi.params[i].type = arg_type
                end
              end
              break
            end
          end
        end
        # Also scan child nodes (receiver, arguments, block)
        scan_call_sites(node.receiver) if node.receiver
        node.arguments&.arguments&.each { |a| scan_call_sites(a) }
        scan_call_sites(node.block) if node.block
      when Prism::ClassNode
        # Track current class context for ivar type resolution
        old_class = @current_class
        cname = node.name.to_s
        @current_class = cname if @classes[cname]
        node.child_nodes.each { |c| scan_call_sites(c) if c }
        @current_class = old_class
      when Prism::DefNode
        # Save and restore var_types_global for method scope isolation
        saved_vars = @var_types_global.dup
        # Pre-populate with method param types if available within current class
        if @current_class && @classes[@current_class]
          ci = @classes[@current_class]
          mi = ci.methods[node.name.to_s]
          if mi
            mi.params.each do |p|
              @var_types_global[p.name] = p.type if p.type != Type::UNKNOWN
            end
          end
        end
        node.child_nodes.each { |c| scan_call_sites(c) if c }
        @var_types_global = saved_vars
      else
        # Generic traversal
        node.child_nodes.each { |c| scan_call_sites(c) if c } if node.respond_to?(:child_nodes)
      end
    end

    # Enhanced type inference for arguments during call site scanning.
    # Uses var_types_global to resolve types that infer_type cannot
    # (e.g., locals in class method bodies that aren't in the current scope).
    def infer_arg_type_for_scan(arg)
      t = infer_type(arg)
      return t if t != Type::UNKNOWN

      if arg.is_a?(Prism::LocalVariableReadNode)
        t = @var_types_global[arg.name.to_s] || Type::UNKNOWN
      elsif arg.is_a?(Prism::CallNode)
        # Handle patterns like arr[i] where arr is in var_types_global
        if arg.name.to_s == "[]" && arg.receiver.is_a?(Prism::LocalVariableReadNode)
          recv_gt = @var_types_global[arg.receiver.name.to_s]
          case recv_gt
          when Type::STR_ARRAY then t = Type::STRING
          when Type::ARRAY then t = Type::INTEGER
          when Type::FLOAT_ARRAY then t = Type::FLOAT
          when Type::HASH then t = Type::INTEGER
          when Type::STR_HASH then t = Type::STRING
          end
        elsif arg.name.to_s == "[]" && arg.receiver.is_a?(Prism::InstanceVariableReadNode) && @current_class
          # Handle @ivar[i] inside class methods
          ci = @classes[@current_class]
          if ci
            ivar = arg.receiver.name.to_s.delete_prefix("@")
            ivar_type = ci.ivars[ivar]
            case ivar_type
            when Type::STR_ARRAY, :str_array then t = Type::STRING
            when Type::ARRAY, :array then t = Type::INTEGER
            when Type::HASH, :hash then t = Type::INTEGER
            end
          end
        elsif %w[split].include?(arg.name.to_s)
          t = Type::STR_ARRAY
        elsif %w[to_i to_int length size count].include?(arg.name.to_s)
          t = Type::INTEGER
        elsif %w[to_s to_str].include?(arg.name.to_s)
          t = Type::STRING
        elsif %w[to_f].include?(arg.name.to_s)
          t = Type::FLOAT
        end
      end
      t
    end

    def scan_ivar_assignments_in_body(node, cname, ci)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| scan_ivar_assignments_in_body(s, cname, ci) }
      when Prism::LocalVariableWriteNode
        # Refresh var_types_global with updated method return types
        t = infer_type(node.value)
        if t == Type::UNKNOWN && node.value.is_a?(Prism::LocalVariableReadNode)
          t = @var_types_global[node.value.name.to_s] || Type::UNKNOWN
        end
        @var_types_global[node.name.to_s] = t if t != Type::UNKNOWN
        scan_ivar_assignments_in_body(node.value, cname, ci)
      when Prism::CallNode
        mname = node.name.to_s
        # Check for attr_name= calls on class instances
        if mname.end_with?("=") && !mname.start_with?("[") && node.receiver && node.arguments
          attr_name = mname.chomp("=")
          if (ci.attrs[:writer]&.include?(attr_name) || ci.attrs[:accessor]&.include?(attr_name))
            # Check if receiver's class matches
            recv_class = nil
            if node.receiver.is_a?(Prism::LocalVariableReadNode)
              recv_class = @var_types_global[node.receiver.name.to_s]
            end
            if recv_class == cname
              node.arguments.arguments.each do |arg|
                arg_type = infer_type(arg)
                if arg_type == Type::UNKNOWN && arg.is_a?(Prism::LocalVariableReadNode)
                  arg_type = @var_types_global[arg.name.to_s] || Type::UNKNOWN
                end
                if arg_type != Type::UNKNOWN && (ci.ivars[attr_name] == Type::NIL || ci.ivars[attr_name] == Type::UNKNOWN)
                  ci.ivars[attr_name] = arg_type
                end
              end
            end
          end
        end
        # Also check @ivar = value inside class methods
        node.child_nodes.each { |c| scan_ivar_assignments_in_body(c, cname, ci) if c }
      when Prism::InstanceVariableWriteNode
        ivar = node.name.to_s.delete_prefix("@")
        if ci.ivars.key?(ivar) && (ci.ivars[ivar] == Type::NIL || ci.ivars[ivar] == Type::UNKNOWN)
          t = infer_type(node.value)
          if t == Type::UNKNOWN && node.value.is_a?(Prism::LocalVariableReadNode)
            # Check method params
            vname = node.value.name.to_s
            ci.methods.each do |_mn, mi|
              mi.params.each do |p|
                if p.name == vname && p.type != Type::UNKNOWN
                  t = p.type
                  break
                end
              end
              break if t != Type::UNKNOWN
            end
            t = @var_types_global[vname] || Type::UNKNOWN if t == Type::UNKNOWN
          end
          if t != Type::UNKNOWN && t != Type::NIL
            ci.ivars[ivar] = t
          end
        end
      else
        if node.respond_to?(:child_nodes)
          node.child_nodes.each { |c| scan_ivar_assignments_in_body(c, cname, ci) if c }
        end
      end
    end

    def propagate_super_types(body, child_init, parent_init, parent_ci)
      return unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.each do |s|
        if (s.is_a?(Prism::CallNode) && s.name.to_s == "super") ||
           s.is_a?(Prism::SuperNode) || s.is_a?(Prism::ForwardingSuperNode)
          args = s.respond_to?(:arguments) && s.arguments ? s.arguments.arguments : []
          args.each_with_index do |arg, i|
            next if i >= parent_init.params.length
            if arg.is_a?(Prism::LocalVariableReadNode)
              # Find this param in child init
              child_param = child_init.params.find { |p| p.name == arg.name.to_s }
              if child_param && child_param.type != Type::UNKNOWN
                parent_init.params[i].type = child_param.type
                pname = parent_init.params[i].name
                parent_ci.ivars.each_key do |iname|
                  if parent_ci.ivars[iname] == Type::UNKNOWN
                    parent_ci.ivars[iname] = child_param.type if param_assigned_to_ivar?(parent_init.body, pname, iname)
                  end
                end
              end
            end
          end
        end
      end
    end

    def param_assigned_to_ivar?(body, pname, iname)
      return false unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.any? do |s|
        s.is_a?(Prism::InstanceVariableWriteNode) &&
          s.name.to_s.delete_prefix("@") == iname &&
          s.value.is_a?(Prism::LocalVariableReadNode) &&
          s.value.name.to_s == pname
      end
    end

    def collect_ivars_from_body(node, ci)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| collect_ivars_from_body(s, ci) }
      when Prism::InstanceVariableWriteNode
        name = node.name.to_s.delete_prefix("@")
        ci.ivars[name] = infer_type(node.value)
      when Prism::IfNode
        collect_ivars_from_body(node.statements, ci)
        collect_ivars_from_body(node.subsequent, ci)
      when Prism::WhileNode
        collect_ivars_from_body(node.statements, ci)
      end
    end

    def infer_body_type(node)
      return Type::VOID unless node
      case node
      when Prism::StatementsNode
        return Type::VOID if node.body.empty?
        last = node.body.last
        # If last statement is an if/while with only side-effect statements, return VOID
        if last.is_a?(Prism::IfNode) || last.is_a?(Prism::WhileNode)
          return Type::VOID if body_is_side_effects?(last)
        end
        # If last statement is a bare return with no value
        if last.is_a?(Prism::ReturnNode) && (last.arguments.nil? || last.arguments.arguments.empty?)
          return Type::VOID
        end
        # If last statement is an array element assignment on a class-typed array (side effect), return VOID
        if last.is_a?(Prism::CallNode) && last.name.to_s == "[]="
          elem_class = array_elem_class_for_receiver(last.receiver)
          return Type::VOID if elem_class
        end
        last_type = infer_type(last)
        # If last expression is a local var read with unknown type, scan body for assignments
        if last_type == Type::UNKNOWN && last.is_a?(Prism::LocalVariableReadNode)
          vname = last.name.to_s
          last_type = infer_local_var_type_from_body(vname, node.body)
        end
        last_type
      else
        infer_type(node)
      end
    end

    # Check if a node tree contains only side-effect statements
    # (assignments, setter calls, method calls with no return value usage)
    def body_is_side_effects?(node)
      return true unless node
      case node
      when Prism::IfNode
        body_is_side_effects?(node.statements) &&
          (node.subsequent.nil? || body_is_side_effects?(node.subsequent))
      when Prism::ElseNode
        body_is_side_effects?(node.statements)
      when Prism::WhileNode
        body_is_side_effects?(node.statements)
      when Prism::StatementsNode
        node.body.all? { |s| stmt_is_side_effect?(s) }
      when Prism::ReturnNode
        node.arguments.nil? || node.arguments.arguments.empty?
      else
        stmt_is_side_effect?(node)
      end
    end

    def stmt_is_side_effect?(node)
      return true unless node
      case node
      when Prism::CallNode
        mname = node.name.to_s
        # Setter calls, puts, print, etc. are side effects
        mname.end_with?("=") || %w[puts print p printf push pop shift unshift].include?(mname) ||
          (node.receiver.nil? && @methods[mname] && @methods[mname].return_type == Type::VOID)
      when Prism::LocalVariableWriteNode, Prism::InstanceVariableWriteNode,
           Prism::LocalVariableOperatorWriteNode, Prism::ConstantWriteNode
        true
      when Prism::IfNode, Prism::WhileNode
        body_is_side_effects?(node)
      when Prism::ReturnNode
        node.arguments.nil? || node.arguments.arguments.empty?
      else
        false
      end
    end

    def infer_local_var_type_from_body(vname, stmts)
      stmts.each do |s|
        if s.is_a?(Prism::LocalVariableWriteNode) && s.name.to_s == vname
          t = infer_type(s.value)
          # Promote STRING to MUTABLE_STRING if this var is used mutably
          if t == Type::STRING && @mutable_string_vars && @mutable_string_vars.include?(vname)
            t = Type::MUTABLE_STRING
          end
          return t if t != Type::UNKNOWN
        end
        # Check inside if/else blocks
        if s.is_a?(Prism::IfNode)
          t = infer_local_var_type_from_body(vname,
            (s.statements.is_a?(Prism::StatementsNode) ? s.statements.body : [s.statements]).compact)
          return t if t != Type::UNKNOWN
        end
      end
      Type::UNKNOWN
    end

    def infer_type(node)
      return Type::NIL unless node
      case node
      when Prism::IntegerNode
        Type::INTEGER
      when Prism::FloatNode
        Type::FLOAT
      when Prism::StringNode, Prism::InterpolatedStringNode
        Type::STRING
      when Prism::SymbolNode
        Type::STRING
      when Prism::XStringNode
        Type::STRING
      when Prism::TrueNode, Prism::FalseNode
        Type::BOOLEAN
      when Prism::NilNode
        Type::NIL
      when Prism::SelfNode
        @current_class ? @current_class : Type::UNKNOWN
      when Prism::ArrayNode
        if node.elements.empty?
          Type::ARRAY  # default to int array
        else
          elem_types = node.elements.map { |e| infer_type(e) }
          elem_types_uniq = elem_types.uniq - [Type::UNKNOWN, Type::NIL]
          if elem_types_uniq.size > 1
            Type::POLY_ARRAY
          elsif elem_types_uniq.size == 1 && elem_types_uniq[0] == Type::STRING
            Type::STR_ARRAY
          else
            Type::ARRAY
          end
        end
      when Prism::HashNode
        # Check values to determine hash type
        if node.elements.any?
          val_types = node.elements.select { |e| e.is_a?(Prism::AssocNode) }.map { |e| infer_type(e.value) }
          val_types_uniq = val_types.uniq - [Type::UNKNOWN, Type::NIL]
          if val_types_uniq.size > 1
            # Mixed value types -> poly hash (string key -> sp_RbValue)
            return Type::POLY_HASH
          end
          if val_types_uniq.size == 1 && val_types_uniq[0] == Type::STRING
            return Type::STR_HASH
          end
        end
        Type::HASH
      when Prism::RangeNode
        Type::RANGE
      when Prism::CallNode
        infer_call_type(node)
      when Prism::LocalVariableReadNode
        v = lookup_var(node.name.to_s)
        v ? v.type : Type::UNKNOWN
      when Prism::ConstantReadNode
        name = node.name.to_s
        if @constants[name]
          @constants[name][:type]
        elsif @current_module && @module_constants["#{@current_module}::#{name}"]
          @module_constants["#{@current_module}::#{name}"][:type]
        else
          Type::UNKNOWN
        end
      when Prism::InstanceVariableReadNode
        if @current_module && @module_ivars && @module_ivars[@current_module]
          ivar = node.name.to_s.delete_prefix("@")
          info = @module_ivars[@current_module][ivar]
          info ? info[:type] : Type::UNKNOWN
        elsif @current_class
          ci = @classes[@current_class]
          ivar = node.name.to_s.delete_prefix("@")
          ci.ivars[ivar] || Type::UNKNOWN
        else
          Type::UNKNOWN
        end
      when Prism::IfNode
        t1 = infer_body_type(node.statements)
        t2 = node.subsequent ? infer_body_type(node.subsequent) : Type::NIL
        t1 == Type::UNKNOWN ? t2 : t1
      when Prism::UnlessNode
        t1 = infer_body_type(node.statements)
        t2 = node.else_clause ? infer_body_type(node.else_clause) : Type::NIL
        t1 == Type::UNKNOWN ? t2 : t1
      when Prism::CaseNode
        infer_case_type(node)
      when Prism::CaseMatchNode
        # Pattern matching: return type of first branch body
        if node.conditions && !node.conditions.empty?
          first = node.conditions.first
          if first.is_a?(Prism::InNode) && first.statements
            infer_body_type(first.statements)
          else
            Type::UNKNOWN
          end
        else
          Type::UNKNOWN
        end
      when Prism::ParenthesesNode
        infer_body_type(node.body)
      when Prism::BeginNode
        if node.rescue_clause || node.ensure_clause
          # Methods with rescue/ensure typically return void or the type of the body
          t = infer_body_type(node.statements)
          t == Type::UNKNOWN ? Type::VOID : t
        else
          infer_body_type(node.statements)
        end
      when Prism::LocalVariableWriteNode
        infer_type(node.value)
      when Prism::InstanceVariableWriteNode
        t = infer_type(node.value)
        if t == Type::UNKNOWN && @current_class
          ivar = node.name.to_s.delete_prefix("@")
          ci = @classes[@current_class]
          t = ci.ivars[ivar] if ci && ci.ivars[ivar] && ci.ivars[ivar] != Type::UNKNOWN
        end
        t || Type::UNKNOWN
      when Prism::NumberedReferenceReadNode
        Type::STRING
      when Prism::RegularExpressionNode
        Type::UNKNOWN  # regex literals are not a value type
      when Prism::MultiWriteNode
        Type::ARRAY
      when Prism::ConstantPathNode
        path = constant_path_str(node)
        if @module_constants[path]
          @module_constants[path][:type]
        else
          Type::UNKNOWN
        end
      else
        Type::UNKNOWN
      end
    end

    def infer_call_type(node)
      mname = node.name.to_s
      recv_type = node.receiver ? infer_type(node.receiver) : nil

      # StringIO methods
      if recv_type == Type::STRINGIO
        case mname
        when "string", "read", "gets", "getc" then return Type::STRING
        when "pos", "tell", "size", "length", "write", "putc", "getbyte", "lineno"
          return Type::INTEGER
        when "eof?", "closed?", "sync", "isatty" then return Type::BOOLEAN
        when "flush" then return Type::STRINGIO
        end
      end

      case mname
      when "+", "-", "*", "/", "%", "**"
        t1 = recv_type || Type::INTEGER
        t2 = node.arguments&.arguments&.first ? infer_type(node.arguments.arguments.first) : Type::INTEGER
        if t1 == Type::POLY || t2 == Type::POLY
          Type::POLY
        elsif (t1 == Type::STRING || t1 == Type::MUTABLE_STRING) && mname == "+"
          Type::STRING
        elsif (t1 == Type::STRING || t1 == Type::MUTABLE_STRING) && mname == "*"
          Type::STRING
        elsif t1 == Type::FLOAT || t2 == Type::FLOAT || mname == "**"
          # ** returns int if both are int in our usage, but for safety:
          if mname == "**"
            Type::INTEGER  # pow returns int for int args in our tests
          else
            Type::FLOAT
          end
        else
          Type::INTEGER
        end
      when "-@", "+@"
        # Unary minus/plus preserves receiver type
        recv_type || Type::INTEGER
      when "==", "!=", "<", ">", "<=", ">=", "&&", "||"
        Type::BOOLEAN
      when "=~"
        Type::BOOLEAN
      when "!", "not"
        Type::BOOLEAN
      when "match?"
        Type::BOOLEAN
      when "itself"
        recv_type || Type::UNKNOWN
      when "succ"
        Type::INTEGER
      when "to_i", "ceil", "floor", "round", "abs", "length", "size", "count", "ord", "hex", "oct", "bytesize", "getbyte", "setbyte"
        if recv_type == Type::FLOAT && mname == "abs"
          Type::FLOAT
        elsif mname == "abs" && recv_type == Type::FLOAT
          Type::FLOAT
        else
          Type::INTEGER
        end
      when "to_s", "to_str", "upcase", "downcase", "strip", "chomp", "chop",
           "reverse", "capitalize", "gsub", "sub", "freeze", "inspect",
           "chars", "join", "name", "class", "to_sym", "ljust", "rjust",
           "center", "lstrip", "rstrip", "tr", "squeeze", "delete", "chr"
        if mname == "chars"
          Type::STR_ARRAY
        elsif mname == "reverse" && (recv_type == Type::ARRAY || recv_type == Type::STR_ARRAY)
          recv_type
        elsif mname == "join"
          Type::STRING
        else
          Type::STRING
        end
      when "to_f", "sqrt", "cos", "sin"
        Type::FLOAT
      when "dup"
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING || recv_type == Type::UNKNOWN || recv_type.nil?
          Type::MUTABLE_STRING
        elsif recv_type == Type::ARRAY
          Type::ARRAY
        elsif recv_type == Type::FLOAT_ARRAY
          Type::FLOAT_ARRAY
        else
          recv_type
        end
      when "to_a", "bytes"
        Type::ARRAY
      when "even?", "odd?", "zero?", "nil?", "empty?", "include?", "frozen?",
           "has_key?", "key?", "start_with?", "end_with?", "is_a?", "respond_to?",
           "positive?", "negative?"
        Type::BOOLEAN
      when "now"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "Time"
          Type::TIME
        else
          Type::UNKNOWN
        end
      when "at"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "Time"
          Type::TIME
        else
          Type::UNKNOWN
        end
      when "read"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "File"
          Type::STRING
        else
          Type::UNKNOWN
        end
      when "exist?", "exists?"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "File"
          Type::BOOLEAN
        else
          Type::BOOLEAN
        end
      when "join"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "File"
          Type::STRING
        else
          Type::STRING
        end
      when "basename"
        Type::STRING
      when "home"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "Dir"
          Type::STRING
        else
          Type::UNKNOWN
        end
      when "[]"
        if node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "ENV"
          Type::STRING
        elsif node.receiver.is_a?(Prism::ConstantReadNode) && node.receiver.name.to_s == "ARGV"
          Type::STRING
        else
          # Check for class-typed array element access
          elem_class = array_elem_class_for_receiver(node.receiver)
          if elem_class
            return elem_class
          end
          # Check for class-typed hash value access
          hash_val_class = hash_value_class_for_receiver(node.receiver)
          if hash_val_class
            return hash_val_class
          end
          recv_type = infer_type(node.receiver)
          case recv_type
          when Type::ARRAY then Type::INTEGER
          when Type::FLOAT_ARRAY then Type::FLOAT
          when Type::STR_ARRAY then Type::STRING
          when Type::HASH then Type::INTEGER
          when Type::STR_HASH then Type::STRING
          when Type::POLY_HASH then Type::POLY
          when Type::STRING then Type::STRING
          when Type::MUTABLE_STRING then Type::STRING
          else Type::UNKNOWN
          end
        end
      when "system"
        Type::BOOLEAN
      when "strip", "chomp", "chop", "lstrip", "rstrip"
        Type::STRING
      when "new"
        if node.receiver.is_a?(Prism::ConstantReadNode)
          cname = node.receiver.name.to_s
          case cname
          when "Array"
            # Detect float arrays: Array.new(n, 0.0) or Array.new(n, float_expr)
            if node.arguments && node.arguments.arguments.length >= 2
              default_val = node.arguments.arguments[1]
              if default_val.is_a?(Prism::FloatNode) || infer_type(default_val) == Type::FLOAT
                Type::FLOAT_ARRAY
              else
                Type::ARRAY
              end
            else
              Type::ARRAY
            end
          when "Hash" then Type::HASH
          when "StringIO" then Type::STRINGIO
          when "Proc" then Type::PROC
          else
            if @classes[cname]
              cname  # class instance type
            else
              Type::UNKNOWN
            end
          end
        else
          Type::UNKNOWN
        end
      when "each", "times", "upto", "downto"
        Type::VOID
      when "map", "collect"
        recv_type == Type::STR_ARRAY ? Type::STR_ARRAY : Type::ARRAY
      when "select", "filter", "reject"
        recv_type == Type::STR_ARRAY ? Type::STR_ARRAY : Type::ARRAY
      when "merge"
        recv_type || Type::HASH
      when "sort", "sort_by", "uniq", "dup", "reverse", "compact", "flatten", "zip"
        recv_type || Type::ARRAY
      when "transform_values"
        recv_type || Type::HASH
      when "keys", "values"
        if recv_type == Type::HASH
          Type::STR_ARRAY  # keys are strings
        else
          Type::ARRAY
        end
      when "push", "<<", "pop", "shift", "unshift", "first", "last", "min", "max", "sum"
        if recv_type == Type::STRING && mname == "<<"
          Type::MUTABLE_STRING
        elsif recv_type == Type::MUTABLE_STRING && mname == "<<"
          Type::MUTABLE_STRING
        elsif %w[first last pop shift].include?(mname) && recv_type == Type::STR_ARRAY
          Type::STRING
        elsif %w[first last min max sum pop shift].include?(mname)
          Type::INTEGER
        else
          recv_type || Type::ARRAY
        end
      when "split"
        Type::STR_ARRAY
      when "[]"
        if recv_type == Type::ARRAY
          Type::INTEGER
        elsif recv_type == Type::STR_ARRAY
          Type::STRING
        elsif recv_type == Type::HASH
          Type::INTEGER
        elsif recv_type == Type::STR_HASH
          Type::STRING
        elsif recv_type == Type::STRING
          Type::STRING
        else
          Type::UNKNOWN
        end
      when "gets"
        Type::STRING
      when "rand"
        # Check if this is a module class method call (e.g., Rand::rand returns float)
        if node.receiver.is_a?(Prism::ConstantReadNode)
          mod_name = node.receiver.name.to_s
          if @module_class_methods && @module_class_methods[mod_name] && @module_class_methods[mod_name][mname]
            mi = @module_class_methods[mod_name][mname]
            return mi.return_type if mi.return_type != Type::UNKNOWN
          end
        end
        Type::INTEGER
      when "format", "sprintf"
        Type::STRING
      when "proc"
        Type::PROC
      when "method"
        Type::UNKNOWN # method objects not fully supported
      when "call"
        if recv_type == Type::PROC
          Type::INTEGER
        else
          Type::INTEGER
        end
      else
        # Check if it's a user-defined method
        if @methods[mname]
          @methods[mname].return_type
        elsif !node.receiver && @current_class && @classes[@current_class]
          # Implicit self call inside a class method
          ci = @classes[@current_class]
          actual = find_method_class(@current_class, mname)
          if actual && @classes[actual].methods[mname]
            @classes[actual].methods[mname].return_type
          else
            Type::UNKNOWN
          end
        elsif recv_type == Type::POLY && @dispatch_methods && @dispatch_methods[mname]
          # Poly dispatch: return type from first implementing class
          @dispatch_methods[mname].each do |cname|
            ci = @classes[cname]
            if ci && ci.methods[mname]
              return ci.methods[mname].return_type
            end
          end
          Type::UNKNOWN
        elsif node.receiver
          # Check class from var_class_types or infer_type (which returns class name string)
          cname = nil
          if node.receiver.is_a?(Prism::LocalVariableReadNode)
            cname = @var_class_types && @var_class_types[node.receiver.name.to_s]
          end
          # Also try infer_type on receiver - it may return a class name string
          if cname.nil?
            rt = recv_type || infer_type(node.receiver)
            cname = rt if rt.is_a?(String) && @classes[rt]
          end
          if cname && @classes[cname]
            ci = @classes[cname]
            actual = find_method_class(cname, mname)
            if actual && @classes[actual].methods[mname]
              @classes[actual].methods[mname].return_type
            elsif ci.ivars[mname]
              ci.ivars[mname]
            else
              Type::UNKNOWN
            end
          elsif node.receiver.is_a?(Prism::ConstantReadNode)
            # Class method call like Point.origin
            rcname = node.receiver.name.to_s
            if @classes[rcname] && @classes[rcname].class_methods[mname]
              @classes[rcname].class_methods[mname].return_type
            else
              Type::UNKNOWN
            end
          else
            Type::UNKNOWN
          end
        else
          Type::UNKNOWN
        end
      end
    end

    def infer_case_type(node)
      return Type::UNKNOWN unless node.conditions && !node.conditions.empty?
      first_when = node.conditions.first
      if first_when.is_a?(Prism::WhenNode) && first_when.statements
        infer_body_type(first_when.statements)
      else
        Type::UNKNOWN
      end
    end

    # ---- Phase 3: Code generation ----
    def generate_module_class_methods
      return unless @module_class_methods
      @module_class_methods.each do |mod_name, methods|
        # Infer return types for module class methods
        methods.each do |mname, mi|
          @current_module = mod_name
          mi.return_type = infer_body_type(mi.body)
          @current_module = nil
        end

        methods.each do |mname, mi|
          rt = c_type(mi.return_type)
          param_str = mi.params.map { |p|
            t = p.type != Type::UNKNOWN ? p.type : Type::INTEGER
            p.type = t
            "#{c_type(t)} lv_#{p.name}"
          }.join(", ")
          param_str = "void" if param_str.empty?

          @forward_decls << "static #{rt} sp_#{mod_name}_#{mname}(#{param_str});"

          @in_main = false
          @current_method = mi
          @current_module = mod_name
          push_scope

          mi.params.each { |p| declare_var(p.name, p.type, c_name: "lv_#{p.name}") }

          old_indent = @indent
          @indent = 1

          emit_raw("")
          emit_raw("static #{rt} sp_#{mod_name}_#{mname}(#{param_str}) {")

          if mi.body
            declare_locals_from_body(mi.body)
            generate_body_return(mi.body, mi.return_type)
          end

          emit_raw("}")

          @indent = old_indent
          pop_scope
          @current_method = nil
          @current_module = nil
          @in_main = true
        end
      end
    end

    def generate_code(root)
      return unless root.is_a?(Prism::ProgramNode)
      stmts = root.statements.body

      # Generate class structs/methods first
      @classes.each { |_name, ci| generate_class(ci) }

      # Generate open class methods for built-in types
      generate_open_class_methods if @open_class_methods

      # Generate dispatch functions for poly class method calls
      generate_dispatch_functions if @dispatch_methods && !@dispatch_methods.empty?

      # Generate module class methods
      generate_module_class_methods

      # Generate top-level method forward declarations and bodies
      @methods.each { |_name, mi| generate_toplevel_method(mi) }

      # Generate main body
      @in_main = true
      @indent = 1
      push_scope

      stmts.each do |s|
        next if s.is_a?(Prism::DefNode) || s.is_a?(Prism::ClassNode) || s.is_a?(Prism::ModuleNode)
        # Skip define_method calls (already collected as methods)
        next if s.is_a?(Prism::CallNode) && s.name.to_s == "define_method"
        generate_stmt(s)
      end

      pop_scope
    end

    def generate_open_class_methods
      @open_class_methods.each do |type_name, methods|
        self_c_type = case type_name
                      when "Integer" then "mrb_int"
                      when "Float" then "mrb_float"
                      when "String" then "const char *"
                      when "Boolean" then "mrb_bool"
                      else "mrb_int"
                      end

        methods.each do |mname, mi|
          # Infer return type from body
          old_class = @current_class
          @current_class = nil  # Not inside a class
          rt = infer_body_type(mi.body)
          @current_class = old_class
          mi.return_type = rt if rt != Type::UNKNOWN

          c_rt = c_type(mi.return_type == Type::UNKNOWN ? Type::INTEGER : mi.return_type)
          cmname = sanitize_method_name(mname)
          params_str = "#{self_c_type} self"
          mi.params.each do |p|
            pt = p.type != Type::UNKNOWN ? p.type : Type::INTEGER
            params_str += ", #{c_type(pt)} lv_#{p.name}"
          end

          @forward_decls << "static #{c_rt} sp_#{type_name}_#{cmname}(#{params_str});"

          @in_main = false
          @current_method = mi
          @current_open_class_type = type_name
          push_scope
          mi.params.each { |p| declare_var(p.name, p.type, c_name: "lv_#{p.name}") }
          old_indent = @indent
          @indent = 1

          emit_raw("")
          emit_raw("static #{c_rt} sp_#{type_name}_#{cmname}(#{params_str}) {")

          if mi.body
            locals = collect_locals(mi.body)
            locals.each do |lname, ltype|
              next if mi.params.any? { |p| p.name == lname }
              declare_var(lname, ltype, c_name: "lv_#{lname}")
              emit("#{c_type(ltype)} lv_#{lname} = #{default_val(ltype)};")
            end
            generate_method_body(mi.body, mi)
          end

          emit_raw("}")
          @current_open_class_type = nil
          @indent = old_indent
          pop_scope
          @current_method = nil
          @in_main = true
        end
      end
    end

    def generate_dispatch_functions
      old_in_main = @in_main
      @in_main = false
      @dispatch_methods.each do |mname, class_set|
        # Determine the return type from the first class that has this method
        ret_type = Type::UNKNOWN
        class_set.each do |cname|
          ci = @classes[cname]
          if ci && ci.methods[mname]
            ret_type = ci.methods[mname].return_type
            break
          end
        end

        rt = c_type(ret_type)
        cmname = sanitize_method_name(mname)

        # Forward declaration
        @forward_decls << "static #{rt} sp_dispatch_#{cmname}(sp_RbValue);"

        # Function body
        emit_raw("")
        emit_raw("static #{rt} sp_dispatch_#{cmname}(sp_RbValue obj) {")
        emit_raw("  uint16_t t = SP_TAG(obj);")
        first = true
        class_set.each do |cname|
          ci = @classes[cname]
          next unless ci && ci.methods[mname]
          prefix = first ? "if" : "else if"
          first = false
          emit_raw("  #{prefix} (t == SP_TAG_#{cname}) return sp_#{cname}_#{cmname}((sp_#{cname} *)sp_unbox_obj(obj));")
        end
        # Default fallback
        emit_raw("  return #{default_val(ret_type)};")
        emit_raw("}")
      end
      @in_main = old_in_main
    end

    def generate_class(ci)
      @needs_gc = true
      name = ci.name

      # Determine if class needs GC allocation (pointer-based)
      needs_gc_alloc = class_needs_gc?(ci)

      # Build ivar list (including inherited)
      all_ivars = collect_all_ivars(ci)

      # Struct declaration
      if needs_gc_alloc
        @struct_decls << "typedef struct sp_#{name}_s sp_#{name};"
        lines = ["struct sp_#{name}_s {"]
        all_ivars.each do |iname, itype|
          elem_class = @ivar_elem_types.dig(name, iname)
          arr_size = @ivar_array_sizes.dig(name, iname)
          if elem_class && arr_size
            eci = @classes[elem_class]
            if eci && class_needs_gc?(eci)
              lines << "  sp_#{elem_class} *#{iname}[#{arr_size}];"
            else
              lines << "  sp_#{elem_class} #{iname}[#{arr_size}];"
            end
          else
            lines << "  #{c_type(itype)} #{iname};"
          end
        end
        lines << "};"
        @struct_decls << lines.join("\n")
      else
        # Value type struct
        @struct_decls << "typedef struct sp_#{name}_s sp_#{name};"
        lines = ["struct sp_#{name}_s {"]
        all_ivars.each do |iname, itype|
          elem_class = @ivar_elem_types.dig(name, iname)
          arr_size = @ivar_array_sizes.dig(name, iname)
          if elem_class && arr_size
            eci = @classes[elem_class]
            if eci && class_needs_gc?(eci)
              lines << "  sp_#{elem_class} *#{iname}[#{arr_size}];"
            else
              lines << "  sp_#{elem_class} #{iname}[#{arr_size}];"
            end
          else
            lines << "  #{c_type(itype)} #{iname};"
          end
        end
        lines << "};"
        @struct_decls << lines.join("\n")
      end

      # Forward declarations for methods
      ci.methods.each do |mname, mi|
        next if mname == "initialize"
        rt = c_type(mi.return_type)
        if needs_gc_alloc
          param_str = "sp_#{name} *"
        else
          param_str = "sp_#{name}"
        end
        mi.params.each do |p|
          param_str += ", #{c_type(resolve_param_type(p, ci, mi))}"
        end
        if mi.has_yield
          param_str += ", sp_block_fn, void *"
        end
        @forward_decls << "static #{rt} sp_#{name}_#{sanitize_method_name(mname)}(#{param_str});"
      end

      ci.class_methods.each do |mname, mi|
        rt = c_type(mi.return_type)
        param_str = mi.params.map { |p| c_type(resolve_param_type(p, ci, mi)) }.join(", ")
        param_str = "void" if param_str.empty?
        @forward_decls << "static #{rt} sp_#{name}_#{sanitize_method_name(mname)}(#{param_str});"
      end

      # Generate GC scan function for classes with pointer ivars
      if needs_gc_alloc
        generate_gc_scan(ci, name, all_ivars)
      end

      # Generate constructor (new)
      generate_constructor(ci, all_ivars, needs_gc_alloc)

      # Generate methods
      ci.methods.each do |mname, mi|
        next if mname == "initialize"
        generate_class_method(ci, mi, all_ivars, needs_gc_alloc)
      end

      # Generate class methods
      ci.class_methods.each do |mname, mi|
        generate_static_class_method(ci, mi)
      end
    end

    def collect_all_ivars(ci)
      ivars = {}
      if ci.parent && @classes[ci.parent]
        parent_ivars = collect_all_ivars(@classes[ci.parent])
        ivars.merge!(parent_ivars)
      end
      ci.ivars.each { |k, v| ivars[k] = v }
      # Also from attrs
      (ci.attrs[:accessor] + ci.attrs[:reader] + ci.attrs[:writer]).uniq.each do |attr|
        ivars[attr] ||= Type::UNKNOWN
      end
      ivars
    end

    def resolve_param_type(param, ci, mi)
      return param.type unless param.type == Type::UNKNOWN

      # Try to infer from initialize body assignments
      if mi.name == "initialize" && mi.body
        infer_param_from_init(param.name, mi.body, ci)
      else
        # Check if param name matches ivar
        if ci.ivars[param.name]
          ci.ivars[param.name]
        elsif %w[<=> < > == != <= >= + - * /].include?(mi.name) && param.name == "other"
          # Operator methods: 'other' param is typically the same class type
          ci.name
        elsif mi.body && param_accesses_class_attr?(mi.body, param.name, ci)
          # If the param is used like `param.attr` where attr is a class ivar/accessor, it's the same class
          ci.name
        elsif mi.body && param_used_as_string?(mi.body, param.name)
          # If param is used with string methods (split, length on string, etc.), infer STRING
          Type::STRING
        else
          Type::INTEGER  # default fallback
        end
      end
    end

    # Check if a parameter is used in string-like contexts in the method body.
    # Looks for: param.split, param.upcase, param + "str", param == "str",
    # strcmp(param, ...), param used as a StrArray element, etc.
    def param_used_as_string?(node, pname)
      return false unless node
      case node
      when Prism::StatementsNode
        node.body.any? { |s| param_used_as_string?(s, pname) }
      when Prism::CallNode
        if node.receiver.is_a?(Prism::LocalVariableReadNode) && node.receiver.name.to_s == pname
          return true if %w[split gsub sub upcase downcase strip chomp chop
                            start_with? end_with? include? index tr
                            length size bytes chars encode force_encoding
                            freeze dup to_i to_f to_s to_sym inspect
                            match? scan replace delete squeeze].include?(node.name.to_s)
          # param[i] where param is used as string
          return true if node.name.to_s == "[]"
          # param + something (string concat)
          return true if node.name.to_s == "+"
          # param == "str" (comparison with string literal)
          if %w[== !=].include?(node.name.to_s) && node.arguments&.arguments&.first
            arg = node.arguments.arguments.first
            return true if arg.is_a?(Prism::StringNode)
          end
        end
        # Check if param is passed to a method that expects a string
        # e.g., emit(param), strcmp(param, something), str_concat(param, ...)
        if node.arguments
          node.arguments.arguments.each do |arg|
            if arg.is_a?(Prism::LocalVariableReadNode) && arg.name.to_s == pname
              # If param is concatenated with a string: result + param
              if node.name.to_s == "+" && node.receiver.is_a?(Prism::LocalVariableReadNode)
                return true
              end
              # Param passed to emit, puts, push on a StrArray, etc.
              return true if %w[emit emit_raw push puts print].include?(node.name.to_s)
            end
          end
        end
        node.child_nodes.each { |c| return true if c && param_used_as_string?(c, pname) }
        false
      when Prism::LocalVariableWriteNode
        param_used_as_string?(node.value, pname)
      when Prism::IfNode
        param_used_as_string?(node.statements, pname) ||
          (node.subsequent && param_used_as_string?(node.subsequent, pname))
      when Prism::WhileNode
        param_used_as_string?(node.statements, pname)
      when Prism::ElseNode
        param_used_as_string?(node.statements, pname)
      else
        # Generic traversal for other node types
        if node.respond_to?(:child_nodes)
          node.child_nodes.any? { |c| c && param_used_as_string?(c, pname) }
        else
          false
        end
      end
    end

    def param_accesses_class_attr?(body, pname, ci)
      return false unless body
      all_attrs = (ci.attrs[:reader] + ci.attrs[:accessor]).uniq
      return false if all_attrs.empty?
      check_param_attr_access(body, pname, all_attrs)
    end

    def check_param_attr_access(node, pname, attrs)
      return false unless node
      case node
      when Prism::CallNode
        if node.receiver.is_a?(Prism::LocalVariableReadNode) && node.receiver.name.to_s == pname
          return true if attrs.include?(node.name.to_s)
        end
        node.child_nodes.each { |c| return true if c && check_param_attr_access(c, pname, attrs) }
        false
      when Prism::StatementsNode
        node.body.any? { |s| check_param_attr_access(s, pname, attrs) }
      else
        if node.respond_to?(:child_nodes)
          node.child_nodes.any? { |c| c && check_param_attr_access(c, pname, attrs) }
        else
          false
        end
      end
    end

    def infer_param_from_init(pname, body, ci)
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.each do |s|
        if s.is_a?(Prism::InstanceVariableWriteNode)
          ivar = s.name.to_s.delete_prefix("@")
          if s.value.is_a?(Prism::LocalVariableReadNode) && s.value.name.to_s == pname
            t = ci.ivars[ivar]
            return t if t && t != Type::UNKNOWN
          end
        end
      end
      # Check ivar type matching param name
      ci.ivars[pname] || Type::INTEGER
    end

    def generate_gc_scan(ci, name, all_ivars)
      # Collect pointer ivars that need marking
      mark_lines = []
      all_ivars.each do |iname, itype|
        elem_class = @ivar_elem_types.dig(name, iname)
        arr_size = @ivar_array_sizes.dig(name, iname)
        if elem_class && arr_size
          eci = @classes[elem_class]
          if eci && class_needs_gc?(eci)
            mark_lines << "  for (int _i = 0; _i < #{arr_size}; _i++) sp_gc_mark(o->#{iname}[_i]);"
          end
        elsif itype.is_a?(String) && @classes[itype] && class_needs_gc?(@classes[itype])
          mark_lines << "  sp_gc_mark(o->#{iname});"
        elsif itype == Type::ARRAY || itype == :int_array
          # IntArrays are GC-allocated and must be marked
          mark_lines << "  if (o->#{iname}) sp_gc_mark(o->#{iname});"
        end
      end
      return if mark_lines.empty?

      lines = []
      lines << "static void sp_#{name}_gc_scan(void *obj) {"
      lines << "  sp_#{name} *o = (sp_#{name} *)obj;"
      lines.concat(mark_lines)
      lines << "}"
      @func_bodies << lines.join("\n")
      @has_gc_scan ||= {}
      @has_gc_scan[name] = true
    end

    def generate_constructor(ci, all_ivars, needs_gc_alloc)
      name = ci.name
      init = ci.methods["initialize"]
      params = init ? init.params : []

      # Inherit parent params if no init defined
      if params.empty? && ci.parent && @classes[ci.parent]
        parent_init = @classes[ci.parent].methods["initialize"]
        params = parent_init ? parent_init.params : []
        init = parent_init  # use parent init for type resolution
      end

      param_str = params.map { |p|
        t = resolve_param_type(p, ci, init || MethodInfo.new)
        "#{c_type(t)} lv_#{p.name}"
      }.join(", ")

      if needs_gc_alloc
        ret_type = "sp_#{name} *"
      else
        ret_type = "sp_#{name}"
      end

      lines = []
      lines << "static #{ret_type} sp_#{name}_new(#{param_str}) {"
      if needs_gc_alloc
        lines << "  SP_GC_SAVE();"
        scan_fn = (@has_gc_scan && @has_gc_scan[name]) ? "sp_#{name}_gc_scan" : "NULL"
        lines << "  sp_#{name} *self = (sp_#{name} *)sp_gc_alloc(sizeof(sp_#{name}), NULL, #{scan_fn});"
        lines << "  SP_GC_ROOT(self);"
      else
        lines << "  sp_#{name} self;"
      end

      # Handle super call in initialize
      if init && init.body
        stmts = init.body.is_a?(Prism::StatementsNode) ? init.body.body : [init.body]
        stmts.each do |s|
          case s
          when Prism::CallNode
            if s.name.to_s == "super" || s.is_a?(Prism::SuperNode) || s.is_a?(Prism::ForwardingSuperNode)
              # super(args) call
              if ci.parent && @classes[ci.parent]
                super_args = s.arguments ? s.arguments.arguments.map { |a|
                  if a.is_a?(Prism::LocalVariableReadNode)
                    "lv_#{a.name}"
                  else
                    compile_expr(a)
                  end
                }.join(", ") : ""
                if needs_gc_alloc
                  lines << "  sp_#{ci.parent}_initialize((sp_#{ci.parent} *)self, #{super_args});"
                else
                  lines << "  sp_#{ci.parent}_initialize(&self, #{super_args});"
                end
              end
            # Handle @ivar[idx] = val in constructor (class-typed array assignment)
            elsif s.name.to_s == "[]=" && s.receiver.is_a?(Prism::InstanceVariableReadNode) &&
               s.arguments && s.arguments.arguments.length == 2
              ivar = s.receiver.name.to_s.delete_prefix("@")
              elem_class = @ivar_elem_types.dig(name, ivar)
              if elem_class
                idx = compile_expr_static(s.arguments.arguments[0])
                val = compile_expr_static(s.arguments.arguments[1])
                accessor = needs_gc_alloc ? "self->#{ivar}" : "self.#{ivar}"
                lines << "  (#{accessor}[#{idx}] = #{val});"
              end
            end
          when Prism::SuperNode, Prism::ForwardingSuperNode
            if ci.parent && @classes[ci.parent]
              super_args = s.respond_to?(:arguments) && s.arguments ? s.arguments.arguments.map { |a|
                if a.is_a?(Prism::LocalVariableReadNode)
                  "lv_#{a.name}"
                else
                  compile_expr(a)
                end
              }.join(", ") : ""
              if needs_gc_alloc
                lines << "  sp_#{ci.parent}_initialize((sp_#{ci.parent} *)self, #{super_args});"
              else
                lines << "  sp_#{ci.parent}_initialize(&self, #{super_args});"
              end
            end
          when Prism::InstanceVariableWriteNode
            ivar = s.name.to_s.delete_prefix("@")
            # Skip Array.new for class-typed array ivars
            elem_class = @ivar_elem_types.dig(name, ivar)
            if elem_class
              # Native C array - skip the Array.new initialization
            else
              val = if s.value.is_a?(Prism::LocalVariableReadNode)
                      "lv_#{s.value.name}"
                    else
                      compile_expr_static(s.value)
                    end
              accessor = needs_gc_alloc ? "self->#{ivar}" : "self.#{ivar}"
              lines << "  #{accessor} = #{val};"
            end
          end
        end
      end

      # For struct classes, generate field assignments from params
      if @struct_classes && @struct_classes[name]
        @struct_classes[name].each do |field|
          accessor = needs_gc_alloc ? "self->#{field}" : "self.#{field}"
          lines << "  #{accessor} = lv_#{field};"
        end
      end

      if needs_gc_alloc
        lines << "  SP_GC_RESTORE();"
        lines << "  return self;"
      else
        lines << "  return self;"
      end
      lines << "}"
      @func_bodies << lines.join("\n")

      # Also generate the initialize function for super calls
      if init && ci.parent && @classes[ci.parent]
        gen_init_func(ci, init, all_ivars, needs_gc_alloc)
      end

      # Generate initialize function if this class has subclasses
      if init && has_subclasses?(ci.name)
        gen_parent_init_func(ci, init, all_ivars, needs_gc_alloc)
      end
    end

    def has_subclasses?(name)
      @classes.any? { |_k, c| c.parent == name }
    end

    def class_needs_gc?(ci)
      has_heap_fields = ci.ivars.any? { |_k, v|
        [Type::ARRAY, Type::FLOAT_ARRAY, Type::STR_ARRAY, Type::HASH, Type::MUTABLE_STRING].include?(v) ||
        (v.is_a?(String) && @classes[v])  # ivar is a class instance type
      }
      uses_inheritance = ci.parent && @classes[ci.parent]
      is_parent_class = has_subclasses?(ci.name)
      is_poly_class = @poly_classes && @poly_classes.include?(ci.name)
      # Classes with non-setter methods that write ivars need pointer semantics
      # (setter methods like x= work fine with value types since they're inlined as field assignment)
      has_ivar_mutation = ci.methods.any? { |mname, mi|
        next false if mname == "initialize"
        next false if mname.end_with?("=")  # setters are inlined, don't need pointer
        mi.body && body_writes_ivar?(mi.body)
      }
      has_heap_fields || uses_inheritance || is_parent_class || is_poly_class || has_ivar_mutation
    end

    def body_writes_ivar?(node)
      return false unless node
      case node
      when Prism::InstanceVariableWriteNode, Prism::InstanceVariableOperatorWriteNode,
           Prism::InstanceVariableAndWriteNode, Prism::InstanceVariableOrWriteNode
        true
      when Prism::StatementsNode
        node.body.any? { |s| body_writes_ivar?(s) }
      when Prism::IfNode, Prism::UnlessNode
        body_writes_ivar?(node.statements) || body_writes_ivar?(node.consequent)
      when Prism::WhileNode, Prism::UntilNode
        body_writes_ivar?(node.statements)
      when Prism::ElseNode
        body_writes_ivar?(node.statements)
      when Prism::LocalVariableWriteNode
        body_writes_ivar?(node.value)
      when Prism::CallNode
        false  # Don't recurse into call nodes
      else
        false
      end
    end

    def gen_parent_init_func(ci, init, _all_ivars, needs_gc_alloc)
      name = ci.name
      param_str = init.params.map { |p|
        t = resolve_param_type(p, ci, init)
        "#{c_type(t)} lv_#{p.name}"
      }.join(", ")

      self_type = "sp_#{name} *self"

      lines = []
      lines << "static void sp_#{name}_initialize(#{self_type}#{param_str.empty? ? '' : ', ' + param_str}) {"
      if init.body
        stmts = init.body.is_a?(Prism::StatementsNode) ? init.body.body : [init.body]
        stmts.each do |s|
          case s
          when Prism::InstanceVariableWriteNode
            ivar = s.name.to_s.delete_prefix("@")
            val = if s.value.is_a?(Prism::LocalVariableReadNode)
                    "lv_#{s.value.name}"
                  else
                    compile_expr_static(s.value)
                  end
            lines << "  self->#{ivar} = #{val};"
          end
        end
      end
      lines << "}"
      @func_bodies << lines.join("\n")
    end

    def gen_init_func(ci, init, _all_ivars, needs_gc_alloc)
      name = ci.name
      parent = ci.parent
      param_str = init.params.map { |p|
        t = resolve_param_type(p, ci, init)
        "#{c_type(t)} lv_#{p.name}"
      }.join(", ")

      if needs_gc_alloc
        self_type = "sp_#{name} *self"
      else
        self_type = "sp_#{name} *self"
      end

      lines = []
      lines << "static void sp_#{name}_initialize(#{self_type}, #{param_str}) {"
      if init.body
        stmts = init.body.is_a?(Prism::StatementsNode) ? init.body.body : [init.body]
        stmts.each do |s|
          case s
          when Prism::InstanceVariableWriteNode
            ivar = s.name.to_s.delete_prefix("@")
            val = if s.value.is_a?(Prism::LocalVariableReadNode)
                    "lv_#{s.value.name}"
                  else
                    compile_expr_static(s.value)
                  end
            lines << "  self->#{ivar} = #{val};"
          end
        end
      end
      lines << "}"
      @func_bodies << lines.join("\n")
    end

    def generate_class_method(ci, mi, _all_ivars, needs_gc_alloc)
      name = ci.name
      mname = mi.name
      rt = c_type(mi.return_type)

      if needs_gc_alloc
        self_param = "sp_#{name} *self"
      else
        self_param = "sp_#{name} self"
      end

      params_list = [self_param] + mi.params.map { |p|
        t = resolve_param_type(p, ci, mi)
        "#{c_type(t)} lv_#{p.name}"
      }

      if mi.has_yield
        @needs_block_fn = true
        params_list << "sp_block_fn _block"
        params_list << "void *_block_env"
      end

      params_str = params_list.join(", ")

      @in_main = false
      @current_class = name
      @current_method = mi
      push_scope

      # Declare params in scope
      mi.params.each do |p|
        t = resolve_param_type(p, ci, mi)
        declare_var(p.name, t, c_name: "lv_#{p.name}")
      end

      old_indent = @indent
      @indent = 1

      lines_before = @func_bodies.length
      emit_raw("static #{rt} sp_#{name}_#{sanitize_method_name(mname)}(#{params_str}) {")

      if needs_gc_alloc
        emit("SP_GC_SAVE();")
        emit("SP_GC_ROOT(self);")
        # Root pointer parameters (class instance types that need GC)
        mi.params.each do |p|
          pt = resolve_param_type(p, ci, mi)
          if pt.is_a?(String) && @classes[pt] && class_needs_gc?(@classes[pt])
            emit("SP_GC_ROOT(lv_#{p.name});")
          end
        end
      end

      # Declare local vars and generate body
      cmp_op = mi.instance_variable_defined?(:@comparable_cmp_op) ? mi.instance_variable_get(:@comparable_cmp_op) : nil
      if cmp_op
        # Synthetic Comparable method: delegate to <=>
        if needs_gc_alloc
          emit("SP_GC_RESTORE();")
        end
        emit("return (sp_#{name}__cmp(self, lv_other) #{cmp_op});")
      elsif mi.body
        declare_locals_from_body(mi.body)
        @gc_restore_before_return = needs_gc_alloc
        generate_body_return(mi.body, mi.return_type)
        @gc_restore_before_return = false
        # Emit SP_GC_RESTORE() at end of body for all GC-rooted methods
        # (not just void -- methods with unknown/integer return also need it
        # when they fall through without an explicit return)
        if needs_gc_alloc
          emit("SP_GC_RESTORE();")
        end
      end

      emit_raw("}")

      @indent = old_indent
      pop_scope
      @current_class = nil
      @current_method = nil
      @in_main = true
    end

    def generate_static_class_method(ci, mi)
      name = ci.name
      mname = mi.name
      rt = c_type(mi.return_type)

      params_str = mi.params.map { |p|
        t = resolve_param_type(p, ci, mi)
        "#{c_type(t)} lv_#{p.name}"
      }.join(", ")
      params_str = "void" if params_str.empty?

      @in_main = false
      @current_class = name
      @current_method = mi
      push_scope
      old_indent = @indent
      @indent = 1

      emit_raw("static #{rt} sp_#{name}_#{sanitize_method_name(mname)}(#{params_str}) {")

      if mi.body
        generate_body_return(mi.body, mi.return_type)
      end

      emit_raw("}")

      @indent = old_indent
      pop_scope
      @current_class = nil
      @current_method = nil
      @in_main = true
    end

    def generate_toplevel_method(mi)
      mname = sanitize_method_name(mi.name)
      rt = c_type(mi.return_type)

      # Check if method takes a block (yield)
      params_list = mi.params.map { |p|
        t = p.type != Type::UNKNOWN ? p.type : infer_param_type_from_calls(mname, p)
        p.type = t
        if p.instance_variable_defined?(:@is_typed_array) && p.instance_variable_get(:@is_typed_array)
          "#{c_type(t)} *lv_#{p.name}"
        else
          "#{c_type(t)} lv_#{p.name}"
        end
      }

      # Handle default parameters
      mi.params.each_with_index do |p, _i|
        if p.default_node && p.type == Type::UNKNOWN
          p.type = infer_type(p.default_node)
        end
      end

      block_param_name = mi.instance_variable_get(:@block_param_name)
      if block_param_name
        @needs_proc = true
        @needs_block_fn = true
        params_list << "sp_Proc *lv_#{block_param_name}"
      elsif mi.has_yield
        @needs_block_fn = true
        params_list << "sp_block_fn _block"
        params_list << "void *_block_env"
      end

      param_str = params_list.join(", ")
      param_str = "void" if param_str.empty?

      # Forward declaration
      @forward_decls << "static #{rt} sp_#{mname}(#{param_str});"

      @in_main = false
      @current_method = mi
      push_scope

      # Declare params
      mi.params.each do |p|
        declare_var(p.name, p.type, c_name: "lv_#{p.name}")
      end
      # Declare block param
      if block_param_name
        declare_var(block_param_name, Type::PROC, c_name: "lv_#{block_param_name}")
      end

      old_indent = @indent
      @indent = 1

      emit_raw("")
      emit_raw("static #{rt} sp_#{mname}(#{param_str}) {")

      # Pre-declare locals
      if mi.body
        locals = collect_locals(mi.body)
        locals.each do |lname, ltype|
          next if mi.params.any? { |p| p.name == lname }
          # Check if this is a class-typed array variable
          elem_class = @array_elem_types[lname]
          arr_size = @local_array_sizes[lname]
          if elem_class && arr_size
            eci = @classes[elem_class]
            declare_var(lname, elem_class, c_name: "lv_#{lname}")
            if eci && class_needs_gc?(eci)
              emit("sp_#{elem_class} *lv_#{lname}[#{arr_size}];")
            else
              emit("sp_#{elem_class} lv_#{lname}[#{arr_size}];")
            end
          else
            declare_var(lname, ltype, c_name: "lv_#{lname}")
            emit("#{c_type(ltype)} lv_#{lname} = #{default_val(ltype)};")
          end
        end

        # GC root management for pointer-typed locals and params
        gc_vars = []
        mi.params.each do |p|
          gc_vars << p.name if [Type::ARRAY, Type::FLOAT_ARRAY, Type::STR_ARRAY, Type::HASH, Type::POLY_HASH].include?(p.type)
          gc_vars << p.name if p.type.is_a?(String) && @classes[p.type] && class_needs_gc?(@classes[p.type])
        end
        locals.each do |lname, ltype|
          next if mi.params.any? { |p| p.name == lname }
          gc_vars << lname if [Type::ARRAY, Type::FLOAT_ARRAY, Type::STR_ARRAY, Type::HASH, Type::POLY_HASH].include?(ltype)
          gc_vars << lname if ltype.is_a?(String) && @classes[ltype] && class_needs_gc?(@classes[ltype])
        end
        unless gc_vars.empty?
          @needs_gc = true
          emit("SP_GC_SAVE();")
          gc_vars.each { |v| emit("SP_GC_ROOT(lv_#{v});") }
        end

        has_gc_roots = !gc_vars.empty?
        @gc_restore_before_return = has_gc_roots
        generate_method_body(mi.body, mi)
        @gc_restore_before_return = false
        # Emit SP_GC_RESTORE() at end of body for all GC-rooted methods
        if has_gc_roots
          emit("SP_GC_RESTORE();")
        end
      end

      emit_raw("}")

      @indent = old_indent
      pop_scope
      @current_method = nil
      @in_main = true
    end

    def infer_param_type_from_calls(method_name, param)
      # Scan main body for calls to this method and infer param type from args
      # For now, use default type or INTEGER
      return param.type if param.type != Type::UNKNOWN
      if param.default_node
        return infer_type(param.default_node)
      end
      Type::INTEGER
    end

    # Promote INTEGER params to FLOAT when the body assigns them to float locals
    # or uses them in float operations.  This fixes mandel_calc(-2,1,1,-1,0.04)
    # where first 4 args are int-literals but used as floats inside the body.
    def promote_params_from_body(mi)
      return unless mi.body
      param_names = mi.params.select { |p| p.type == Type::INTEGER }.map(&:name).to_set
      return if param_names.empty?

      # Build a map of local var -> types assigned to it (considering += float etc.)
      local_types = {}
      collect_local_types(mi.body, local_types)

      # Also incorporate var_types_global for locals that were upgraded via +=
      local_types.each_key do |vname|
        gt = @var_types_global[vname]
        local_types[vname] << gt if gt && gt != Type::UNKNOWN
      end

      # Find float locals
      float_locals = Set.new
      local_types.each do |vname, types|
        float_locals << vname if types.include?(Type::FLOAT)
      end

      # Check which params are assigned to float locals
      promoted = Set.new
      scan_param_float_usage(mi.body, param_names, float_locals, promoted)

      promoted.each do |pname|
        p = mi.params.find { |pp| pp.name == pname }
        p.type = Type::FLOAT if p
      end
    end

    def collect_local_types(node, map)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| collect_local_types(s, map) }
      when Prism::LocalVariableWriteNode
        t = infer_type(node.value)
        map[node.name.to_s] ||= Set.new
        map[node.name.to_s] << t if t != Type::UNKNOWN
        collect_local_types(node.value, map)
      when Prism::LocalVariableOperatorWriteNode
        t = infer_type(node.value)
        map[node.name.to_s] ||= Set.new
        map[node.name.to_s] << t if t != Type::UNKNOWN
      when Prism::WhileNode
        collect_local_types(node.statements, map)
      when Prism::IfNode
        collect_local_types(node.statements, map)
        collect_local_types(node.subsequent, map) if node.respond_to?(:subsequent)
      else
        node.child_nodes.each { |c| collect_local_types(c, map) if c } if node.respond_to?(:child_nodes)
      end
    end

    def scan_param_float_usage(node, param_names, float_locals, promoted)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| scan_param_float_usage(s, param_names, float_locals, promoted) }
      when Prism::LocalVariableWriteNode
        # local_var = param  where local_var is float
        vname = node.name.to_s
        if float_locals.include?(vname) && node.value.is_a?(Prism::LocalVariableReadNode)
          ref = node.value.name.to_s
          promoted << ref if param_names.include?(ref)
        end
        # Check if value expression uses param in float context (e.g., param * 255.5)
        scan_param_float_expr(node.value, param_names, float_locals, promoted)
        scan_param_float_usage(node.value, param_names, float_locals, promoted)
      when Prism::CallNode
        # Check arithmetic: param <op> float_literal or float_local <op> param
        scan_param_float_expr(node, param_names, float_locals, promoted)
      when Prism::WhileNode
        # while (cur_i > max_i) - if cur_i is float and max_i is param, promote max_i
        if node.predicate.is_a?(Prism::CallNode) &&
           %w[> < >= <= == !=].include?(node.predicate.name.to_s)
          recv = node.predicate.receiver
          arg = node.predicate.arguments&.arguments&.first
          if recv.is_a?(Prism::LocalVariableReadNode) && float_locals.include?(recv.name.to_s) &&
             arg.is_a?(Prism::LocalVariableReadNode) && param_names.include?(arg.name.to_s)
            promoted << arg.name.to_s
          end
          if arg.is_a?(Prism::LocalVariableReadNode) && float_locals.include?(arg.name.to_s) &&
             recv.is_a?(Prism::LocalVariableReadNode) && param_names.include?(recv.name.to_s)
            promoted << recv.name.to_s
          end
        end
        scan_param_float_usage(node.statements, param_names, float_locals, promoted)
      when Prism::IfNode
        scan_param_float_usage(node.statements, param_names, float_locals, promoted)
        scan_param_float_usage(node.subsequent, param_names, float_locals, promoted) if node.respond_to?(:subsequent)
      else
        node.child_nodes.each { |c| scan_param_float_usage(c, param_names, float_locals, promoted) if c } if node.respond_to?(:child_nodes)
      end
    end

    # Detect param used in float arithmetic: param * 255.5, param + float_local, etc.
    def scan_param_float_expr(node, param_names, float_locals, promoted)
      return unless node.is_a?(Prism::CallNode)
      mname = node.name.to_s
      return unless %w[+ - * /].include?(mname) && node.receiver && node.arguments
      recv = node.receiver
      arg = node.arguments.arguments&.first
      return unless arg
      # Check if one side is a param and the other is float
      recv_is_param = recv.is_a?(Prism::LocalVariableReadNode) && param_names.include?(recv.name.to_s)
      arg_is_param = arg.is_a?(Prism::LocalVariableReadNode) && param_names.include?(arg.name.to_s)
      recv_is_float = recv.is_a?(Prism::FloatNode) ||
                      (recv.is_a?(Prism::LocalVariableReadNode) && float_locals.include?(recv.name.to_s))
      arg_is_float = arg.is_a?(Prism::FloatNode) ||
                     (arg.is_a?(Prism::LocalVariableReadNode) && float_locals.include?(arg.name.to_s))
      if recv_is_param && arg_is_float
        promoted << recv.name.to_s
      end
      if arg_is_param && recv_is_float
        promoted << arg.name.to_s
      end
      # Recurse into sub-expressions
      scan_param_float_expr(recv, param_names, float_locals, promoted) if recv.is_a?(Prism::CallNode)
      scan_param_float_expr(arg, param_names, float_locals, promoted) if arg.is_a?(Prism::CallNode)
    end

    def generate_method_body(body, mi)
      return unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.each_with_index do |s, i|
        is_last = (i == stmts.length - 1)
        if is_last && mi.return_type != Type::VOID
          # Last statement is the return value
          generate_return_stmt(s, mi)
        else
          generate_stmt(s)
        end
      end
    end

    def generate_return_stmt(node, mi)
      case node
      when Prism::BeginNode
        generate_begin_stmt(node)
      when Prism::IfNode
        generate_if_stmt(node, return_type: mi.return_type)
      when Prism::CaseNode
        generate_case_stmt(node, return_type: mi.return_type)
      when Prism::CaseMatchNode
        val = compile_case_match_expr(node)
        emit_gc_return(val)
      when Prism::ReturnNode
        if node.arguments && node.arguments.arguments.length > 0
          val = compile_expr(node.arguments.arguments.first)
          emit_gc_return(val)
        else
          if @gc_restore_before_return
            emit("SP_GC_RESTORE();")
          end
          emit("return;")
        end
      when Prism::CallNode
        if node.name.to_s == "puts" || node.name.to_s == "print" || node.name.to_s == "p"
          generate_stmt(node)
          # For void-ish last statements, don't return
        else
          val = compile_expr(node)
          if mi.return_type != Type::VOID
            emit_gc_return(val)
          else
            emit("#{val};")
          end
        end
      when Prism::ArrayNode
        # Return array literal
        val = compile_expr(node)
        emit_gc_return(val)
      when Prism::WhileNode
        generate_stmt(node)
      else
        val = compile_expr(node)
        if val && val != "" && mi.return_type != Type::VOID
          emit_gc_return(val)
        else
          generate_stmt(node) if val.nil? || val == ""
        end
      end
    end

    def generate_body_stmts(body, is_return_context: false)
      return unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.each { |s| generate_stmt(s) }
    end

    def generate_body_return(body, return_type)
      return unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.each_with_index do |s, i|
        if i == stmts.length - 1 && return_type != Type::VOID
          # Statement-only nodes cannot be compiled as expressions
          case s
          when Prism::WhileNode, Prism::UntilNode, Prism::ForNode
            generate_stmt(s)
          when Prism::IfNode
            generate_if_stmt(s, return_type: return_type)
          when Prism::CaseNode
            generate_case_stmt(s, return_type: return_type)
          when Prism::BeginNode
            generate_begin_stmt(s)
          when Prism::ReturnNode
            if s.arguments && s.arguments.arguments.length > 0
              val = compile_expr(s.arguments.arguments.first)
              emit_gc_return(val)
            else
              if @gc_restore_before_return
                emit("SP_GC_RESTORE();")
              end
              emit("return;")
            end
          when Prism::CallNode
            if s.name.to_s == "puts" || s.name.to_s == "print" || s.name.to_s == "p"
              generate_stmt(s)
            else
              val = compile_expr(s)
              if return_type != Type::VOID
                emit_gc_return(val)
              else
                emit("#{val};")
              end
            end
          else
            val = compile_expr(s)
            if val && val != "" && return_type != Type::VOID
              emit_gc_return(val)
            else
              generate_stmt(s) if val.nil? || val == ""
            end
          end
        else
          generate_stmt(s)
        end
      end
    end

    # ---- Statement generation ----
    def generate_stmt(node)
      return unless node
      case node
      when Prism::LocalVariableWriteNode
        generate_local_write(node)
      when Prism::LocalVariableOperatorWriteNode
        generate_local_op_write(node)
      when Prism::InstanceVariableWriteNode
        generate_ivar_write(node)
      when Prism::ConstantWriteNode
        generate_const_write(node)
      when Prism::ConstantPathWriteNode
        # module::CONST
        generate_stmt(node.value)
      when Prism::MultiWriteNode
        generate_multi_write(node)
      when Prism::CallNode
        generate_call_stmt(node)
      when Prism::IfNode
        generate_if_stmt(node)
      when Prism::UnlessNode
        generate_unless_stmt(node)
      when Prism::WhileNode
        generate_while_stmt(node)
      when Prism::UntilNode
        generate_until_stmt(node)
      when Prism::ForNode
        generate_for_stmt(node)
      when Prism::CaseNode
        generate_case_stmt(node)
      when Prism::CaseMatchNode
        generate_case_match_stmt(node)
      when Prism::BeginNode
        generate_begin_stmt(node)
      when Prism::ReturnNode
        generate_return(node)
      when Prism::BreakNode
        emit("break;")
      when Prism::NextNode
        emit("continue;")
      when Prism::RetryNode
        # retry jumps back to the begin label
        if @current_retry_label
          emit("goto #{@current_retry_label};")
        end
      when Prism::DefNode, Prism::ClassNode, Prism::ModuleNode
        # already handled
      when Prism::StatementsNode
        node.body.each { |s| generate_stmt(s) }
      when Prism::ParenthesesNode
        if node.body
          generate_stmt(node.body)
        end
      when Prism::GlobalVariableWriteNode
        # $var = value
        gname = node.name.to_s.delete_prefix("$")
        val = compile_expr(node.value)
        emit("/* global $#{gname} = #{val} */")
      when Prism::YieldNode
        generate_yield(node)
      when Prism::RescueModifierNode
        generate_rescue_modifier(node)
      else
        # Try to compile as expression statement
        val = compile_expr(node)
        emit("#{val};") if val && val != ""
      end
    end

    def generate_local_write(node)
      name = node.name.to_s
      val_type = infer_type(node.value)
      existing = lookup_var(name)

      # Skip Array.new(N) assignment for class-typed array variables
      if @array_elem_types[name] && @local_array_sizes[name] &&
         node.value.is_a?(Prism::CallNode) && node.value.name.to_s == "new" &&
         node.value.receiver.is_a?(Prism::ConstantReadNode) && node.value.receiver.name.to_s == "Array"
        return
      end

      # Check if this variable is polymorphic
      is_poly = @poly_vars && @poly_vars.include?(name)
      if is_poly
        val_type = Type::POLY
        @needs_poly = true
      end

      # Check if this is a mutable string variable
      if !is_poly && val_type == Type::STRING && @mutable_string_vars && @mutable_string_vars.include?(name)
        val_type = Type::MUTABLE_STRING
        @needs_mutable_string = true
      end

      # Track class instance types
      if node.value.is_a?(Prism::CallNode) && node.value.name.to_s == "new" && node.value.receiver.is_a?(Prism::ConstantReadNode)
        cname = node.value.receiver.name.to_s
        if @classes[cname]
          @var_class_types ||= {}
          @var_class_types[name] = cname
        end
      end
      # Also track when val_type is a class name string (from method return types)
      if val_type.is_a?(String) && @classes[val_type]
        @var_class_types ||= {}
        @var_class_types[name] = val_type
      end

      # Track method(:name) assignments
      if node.value.is_a?(Prism::CallNode) && node.value.name.to_s == "method"
        margs = call_args(node.value)
        if margs.length > 0 && margs[0].is_a?(Prism::SymbolNode)
          @method_refs[name] = margs[0].value
          emit("/* #{name} = method(:#{margs[0].value}) */")
          return
        end
      end

      # Check if this is a mutable string (s = "hello" followed by s << ...)
      # We detect this heuristically - if value is string but used with <<

      c_name = "lv_#{name}"

      if existing
        # Already declared
        val = compile_expr(node.value)
        # Wrap string literal for mutable string
        if val_type == Type::MUTABLE_STRING && node.value.is_a?(Prism::StringNode)
          val = "sp_String_new(#{val})"
        end
        # Box value for poly vars
        if is_poly
          val = box_value(val, infer_type(node.value))
        end
        emit("#{c_name} = #{val};")
        # Update type if needed
        if existing.type != val_type && val_type != Type::UNKNOWN
          existing.type = val_type
        end
      else
        # New variable
        info = declare_var(name, val_type, c_name: c_name)
        val = compile_expr(node.value)
        # Wrap string literal for mutable string
        if val_type == Type::MUTABLE_STRING && node.value.is_a?(Prism::StringNode)
          val = "sp_String_new(#{val})"
        end
        # Box value for poly vars
        if is_poly
          val = box_value(val, infer_type(node.value))
        end

        if @in_main
          # Declare in main header
          class_name = @var_class_types && @var_class_types[name]
          @main_vars << { name: c_name, type: val_type, class_name: class_name }
          emit("#{c_name} = #{val};")
        else
          # Already pre-declared or declare inline
          emit("#{c_name} = #{val};")
        end
      end
    end

    def generate_local_op_write(node)
      name = node.name.to_s
      op = node.binary_operator.to_s
      c_name = "lv_#{name}"
      val = compile_expr(node.value)
      var_info = lookup_var(name)
      var_type = var_info ? var_info.type : Type::UNKNOWN
      val_type = infer_type(node.value)
      if op == "/" && var_type == Type::INTEGER && val_type == Type::INTEGER
        emit("#{c_name} = sp_idiv(#{c_name}, #{val});")
      elsif op == "%" && var_type == Type::INTEGER && val_type == Type::INTEGER
        emit("#{c_name} = sp_imod(#{c_name}, #{val});")
      else
        emit("#{c_name} #{op}= #{val};")
      end
    end

    def generate_ivar_write(node)
      ivar = node.name.to_s.delete_prefix("@")
      val = compile_expr(node.value)
      if @current_module
        emit("sp_#{@current_module}_#{ivar} = #{val};")
      elsif @current_class
        ci = @classes[@current_class]
        if class_needs_gc?(ci)
          emit("self->#{ivar} = #{val};")
        else
          emit("self.#{ivar} = #{val};")
        end
      end
    end

    def generate_const_write(node)
      name = node.name.to_s
      # Skip struct class constants (handled as class definitions, not regular constants)
      return if @struct_classes && @struct_classes[name]
      return if @classes[name]
      return unless @constants[name]
      type = @constants[name][:type]
      val = compile_expr(node.value)
      emit("cv_#{name} = #{val};")
    end

    def generate_multi_write(node)
      targets = node.lefts
      value = node.value

      case value
      when Prism::ArrayNode
        # a, b, c = [1, 2, 3]
        emit("{")
        @indent += 1
        value.elements.each_with_index do |elem, i|
          next if i >= targets.length
          tmp = "_mw_#{i}"
          val = compile_expr(elem)
          t = infer_type(elem)
          emit("#{c_type(t)} #{tmp} = #{val};")
        end
        targets.each_with_index do |target, i|
          next if i >= value.elements.length
          if target.is_a?(Prism::InstanceVariableTargetNode)
            ivar = target.name.to_s.delete_prefix("@")
            if @current_module
              emit("sp_#{@current_module}_#{ivar} = _mw_#{i};")
            elsif @current_class
              ci = @classes[@current_class]
              needs_ptr = class_needs_gc?(ci)
              accessor = needs_ptr ? "self->#{ivar}" : "self.#{ivar}"
              emit("#{accessor} = _mw_#{i};")
            end
          else
            tname = target_name(target)
            ensure_var_declared(tname, infer_type(value.elements[i]))
            emit("lv_#{tname} = _mw_#{i};")
          end
        end
        @indent -= 1
        emit("}")
      when Prism::CallNode
        # q, r = some_method(args)
        val = compile_expr(value)
        tmp = "_mv_#{next_temp}"
        arr_type = infer_type(value)
        if arr_type == Type::ARRAY
          @needs_int_array = true
          @needs_gc = true
          emit("{ sp_IntArray *#{tmp} = #{val};")
          targets.each_with_index do |target, i|
            tname = target_name(target)
            ensure_var_declared(tname, Type::INTEGER)
            emit("  lv_#{tname} = sp_IntArray_get(#{tmp}, #{i});")
          end
          emit("}")
        else
          emit("/* multi-assign from call */")
          emit("#{val};")
        end
      when Prism::LocalVariableReadNode
        # a, b = b, a (swap) - this is actually multi_write with splat_node
        # In prism, swaps look like: targets = [a, b], value = [b, a]
        emit("/* swap */")
      else
        val = compile_expr(value)
        emit("/* multi-assign: #{val} */")
      end
    end

    def target_name(node)
      case node
      when Prism::LocalVariableTargetNode
        node.name.to_s
      when Prism::LocalVariableWriteNode
        node.name.to_s
      else
        "unknown"
      end
    end

    def ensure_var_declared(name, type)
      existing = lookup_var(name)
      unless existing
        declare_var(name, type, c_name: "lv_#{name}")
        if @in_main
          @main_vars << { name: "lv_#{name}", type: type }
        else
          # Inside a function - emit the declaration directly
          emit("#{c_type(type)} lv_#{name} = #{default_val(type)};")
        end
      end
    end

    def generate_call_stmt(node)
      mname = node.name.to_s

      # Check for receiver-based calls
      if node.receiver
        val = compile_expr(node)
        emit("#{val};") unless val.nil? || val == "" || val == "0"
        return
      end

      case mname
      when "puts"
        generate_puts(node)
      when "print"
        generate_print(node)
      when "p"
        generate_p_call(node)
      when "printf"
        generate_printf(node)
      when "raise"
        generate_raise(node)
      when "loop"
        generate_loop(node)
      when "require", "require_relative"
        # skip
      when "freeze"
        # no-op
      else
        # Check for modifier if/unless
        if node.block.nil?
          val = compile_expr(node)
          emit("#{val};")
        else
          # Block call
          val = compile_expr(node)
          emit("#{val};") unless val.nil? || val == ""
        end
      end
    end

    def generate_puts(node)
      args = call_args(node)
      if args.empty?
        emit('putchar(\'\\n\');')
        return
      end

      args.each do |arg|
        type = infer_type(arg)
        val = compile_expr(arg)

        # Check if the variable is poly
        if arg.is_a?(Prism::LocalVariableReadNode)
          v = lookup_var(arg.name.to_s)
          if v && v.type == Type::POLY
            type = Type::POLY
          end
        end

        case type
        when Type::POLY
          @needs_poly = true
          emit("sp_poly_puts(#{val});")
        when Type::INTEGER
          emit("printf(\"%lld\\n\", (long long)#{val});")
        when Type::FLOAT
          emit("{ const char *_fs = sp_float_to_s(#{val}); printf(\"%s\\n\", _fs); }")
          @string_helpers_needed << :float_to_s
        when Type::BOOLEAN
          emit("puts(#{val} ? \"true\" : \"false\");")
        when Type::STRING, Type::SYMBOL
          emit("{ const char *_ps = #{val}; if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); } else putchar('\\n'); }")
        when Type::MUTABLE_STRING
          emit("{ const char *_ps = sp_String_cstr(#{val}); fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }")
          @needs_mutable_string = true
        when Type::NIL
          emit("puts(\"\");")
        else
          # Try to guess
          if val =~ /strlen|sp_str_|"[^"]*"/
            emit("{ const char *_ps = #{val}; fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }")
          else
            emit("printf(\"%lld\\n\", (long long)#{val});")
          end
        end
      end
    end

    def generate_print(node)
      args = call_args(node)
      args.each do |arg|
        type = infer_type(arg)
        # Special case: print x.chr -> putchar(x) for binary safety
        if arg.is_a?(Prism::CallNode) && arg.name.to_s == "chr"
          recv = compile_expr(arg.receiver)
          emit("putchar((char)(#{recv}));")
          next
        end
        val = compile_expr(arg)
        case type
        when Type::INTEGER
          emit("printf(\"%lld\", (long long)#{val});")
        when Type::FLOAT
          emit("printf(\"%g\", #{val});")
        when Type::STRING
          emit("fputs(#{val}, stdout);")
        when Type::BOOLEAN
          emit("fputs(#{val} ? \"true\" : \"false\", stdout);")
        else
          emit("printf(\"%lld\", (long long)#{val});")
        end
      end
    end

    def generate_p_call(node)
      args = call_args(node)
      args.each do |arg|
        type = infer_type(arg)
        val = compile_expr(arg)
        case type
        when Type::INTEGER
          emit("printf(\"%lld\\n\", (long long)#{val});")
        when Type::STRING
          emit("printf(\"\\\"%s\\\"\\n\", #{val});")
        when Type::BOOLEAN
          emit("puts(#{val} ? \"true\" : \"false\");")
        when Type::NIL
          emit("puts(\"nil\");")
        else
          emit("printf(\"%lld\\n\", (long long)#{val});")
        end
      end
    end

    def generate_printf(node)
      args = call_args(node)
      return if args.empty?
      # Special case: printf("%c", val) -> sp_putc_utf8(val) for CRuby UTF-8 compat
      if args.length == 2 && args[0].is_a?(Prism::StringNode) && args[0].content == "%c"
        val = compile_expr(args[1])
        emit("sp_putc_utf8(#{val});")
        return
      end
      fmt = compile_expr(args[0])
      rest = args[1..].map { |a| compile_expr(a) }.join(", ")
      if rest.empty?
        emit("printf(#{fmt});")
      else
        emit("printf(#{fmt}, #{rest});")
      end
    end

    def generate_raise(node)
      @needs_exception = true
      args = call_args(node)
      if args.empty?
        emit('sp_raise("RuntimeError");')
      elsif args.length == 1
        arg = args[0]
        if arg.is_a?(Prism::CallNode) && arg.name.to_s == "new" && arg.receiver.is_a?(Prism::ConstantReadNode)
          # raise ClassName.new("msg")
          cls_name = c_string_literal(arg.receiver.name.to_s)
          msg_args = call_args(arg)
          msg = msg_args.length > 0 ? compile_expr(msg_args[0]) : c_string_literal(arg.receiver.name.to_s)
          emit("sp_raise_cls(#{cls_name}, #{msg});")
        else
          val = compile_expr(arg)
          emit("sp_raise(#{val});")
        end
      else
        # raise ClassName, "message"
        cls_arg = args[0]
        cls_name = cls_arg.is_a?(Prism::ConstantReadNode) ? c_string_literal(cls_arg.name.to_s) : compile_expr(cls_arg)
        msg = compile_expr(args[1])
        emit("sp_raise_cls(#{cls_name}, #{msg});")
      end
    end

    def generate_loop(node)
      return unless node.block
      emit("while (1) {")
      @indent += 1
      if node.block.body
        generate_body_stmts(node.block.body)
      end
      @indent -= 1
      emit("}")
    end

    # Compile the last statement of a branch into res_var, handling
    # statement-only nodes (WhileNode, etc.) that cannot be expressions.
    def compile_last_into(s, res_var)
      case s
      when Prism::WhileNode, Prism::UntilNode, Prism::ForNode
        generate_stmt(s)
      else
        val = compile_expr(s)
        emit("#{res_var} = #{val};")
      end
    end

    def generate_if_stmt(node, return_type: nil)
      cond = compile_expr(node.predicate)

      # Check for modifier if (no consequent)
      if node.is_a?(Prism::IfNode)
        # Handle modifier unless at statement level
      end

      if return_type
        # Generate as expression with result variable
        res_var = "_cres_#{next_temp}"
        emit("#{c_type(return_type)} #{res_var} = #{default_val(return_type)};")
        emit("if (#{cond}) {")
        @indent += 1
        if node.statements
          stmts = node.statements.is_a?(Prism::StatementsNode) ? node.statements.body : [node.statements]
          stmts.each_with_index do |s, i|
            if i == stmts.length - 1
              compile_last_into(s, res_var)
            else
              generate_stmt(s)
            end
          end
        end
        @indent -= 1
        emit("}")
        if node.subsequent
          generate_elsif_chain(node.subsequent, res_var, return_type)
        end
        emit_gc_return(res_var)
      else
        emit("if (#{cond}) {")
        @indent += 1
        generate_body_stmts(node.statements)
        @indent -= 1
        emit("}")
        if node.subsequent
          generate_else_chain(node.subsequent)
        end
      end
    end

    def generate_else_chain(node)
      case node
      when Prism::ElseNode
        emit("else {")
        @indent += 1
        generate_body_stmts(node.statements)
        @indent -= 1
        emit("}")
      when Prism::IfNode
        cond = compile_expr(node.predicate)
        emit("else if (#{cond}) {")
        @indent += 1
        generate_body_stmts(node.statements)
        @indent -= 1
        emit("}")
        if node.subsequent
          generate_else_chain(node.subsequent)
        end
      else
        emit("}")
      end
    end

    def generate_elsif_chain(node, res_var, return_type)
      case node
      when Prism::ElseNode
        emit("else {")
        @indent += 1
        if node.statements
          stmts = node.statements.is_a?(Prism::StatementsNode) ? node.statements.body : [node.statements]
          stmts.each_with_index do |s, i|
            if i == stmts.length - 1
              compile_last_into(s, res_var)
            else
              generate_stmt(s)
            end
          end
        end
        @indent -= 1
        emit("}")
      when Prism::IfNode
        cond = compile_expr(node.predicate)
        emit("else if (#{cond}) {")
        @indent += 1
        if node.statements
          stmts = node.statements.is_a?(Prism::StatementsNode) ? node.statements.body : [node.statements]
          stmts.each_with_index do |s, i|
            if i == stmts.length - 1
              compile_last_into(s, res_var)
            else
              generate_stmt(s)
            end
          end
        end
        @indent -= 1
        emit("}")
        if node.subsequent
          generate_elsif_chain(node.subsequent, res_var, return_type)
        end
      else
        emit("}")
      end
    end

    def generate_unless_stmt(node)
      cond = compile_expr(node.predicate)
      emit("if (!(#{cond})) {")
      @indent += 1
      generate_body_stmts(node.statements)
      @indent -= 1
      if node.else_clause
        emit("else {")
        @indent += 1
        generate_body_stmts(node.else_clause.statements)
        @indent -= 1
        emit("}")
      else
        emit("}")
      end
    end

    def generate_while_stmt(node)
      cond = compile_expr(node.predicate)
      emit("while (#{cond}) {")
      @indent += 1
      generate_body_stmts(node.statements)
      @indent -= 1
      emit("}")
    end

    def generate_until_stmt(node)
      cond = compile_expr(node.predicate)
      emit("while (!(#{cond})) {")
      @indent += 1
      generate_body_stmts(node.statements)
      @indent -= 1
      emit("}")
    end

    def generate_for_stmt(node)
      # for i in range
      var_name = case node.index
                 when Prism::LocalVariableTargetNode then node.index.name.to_s
                 else "i"
                 end

      ensure_var_declared(var_name, Type::INTEGER)

      collection = node.collection
      case collection
      when Prism::RangeNode
        @needs_range = true
        first = compile_expr(collection.left)
        last = compile_expr(collection.right)
        emit("for (lv_#{var_name} = #{first}; lv_#{var_name} <= #{last}; lv_#{var_name}++) {")
      when Prism::CallNode
        # for i in arr
        val = compile_expr(collection)
        emit("for (mrb_int _fi = 0; _fi < sp_IntArray_length(#{val}); _fi++) {")
        emit("  lv_#{var_name} = sp_IntArray_get(#{val}, _fi);")
        @needs_int_array = true
        @needs_gc = true
      else
        val = compile_expr(collection)
        emit("/* for #{var_name} in #{val} */")
        emit("for (mrb_int _fi = 0; _fi < sp_IntArray_length(#{val}); _fi++) {")
        emit("  lv_#{var_name} = sp_IntArray_get(#{val}, _fi);")
      end

      @indent += 1
      generate_body_stmts(node.statements)
      @indent -= 1
      emit("}")
    end

    def generate_case_match_stmt(node)
      # case val; in Pattern; body; end
      pred = compile_expr(node.predicate)
      pred_type = infer_type(node.predicate)
      # Check if predicate var is poly
      if node.predicate.is_a?(Prism::LocalVariableReadNode)
        v = lookup_var(node.predicate.name.to_s)
        pred_type = Type::POLY if v && v.type == Type::POLY
      end

      first = true
      (node.conditions || []).each do |cond|
        next unless cond.is_a?(Prism::InNode)
        check = compile_pattern_check(cond.pattern, pred, pred_type)
        if first
          emit("if (#{check}) {")
          first = false
        else
          emit("else if (#{check}) {")
        end
        @indent += 1
        if cond.statements
          stmts = cond.statements.is_a?(Prism::StatementsNode) ? cond.statements.body : [cond.statements]
          stmts.each { |s| generate_stmt(s) }
        end
        @indent -= 1
        emit("}")
      end
      if node.else_clause
        emit("else {")
        @indent += 1
        generate_stmt(node.else_clause.statements)
        @indent -= 1
        emit("}")
      end
    end

    def compile_case_match_expr(node)
      pred = compile_expr(node.predicate)
      pred_type = infer_type(node.predicate)
      if node.predicate.is_a?(Prism::LocalVariableReadNode)
        v = lookup_var(node.predicate.name.to_s)
        pred_type = Type::POLY if v && v.type == Type::POLY
      end

      result_tmp = "_cm_#{next_temp}"
      # Determine result type
      first_body = node.conditions&.first
      result_type = first_body && first_body.is_a?(Prism::InNode) && first_body.statements ?
        infer_body_type(first_body.statements) : Type::STRING
      emit("#{c_type(result_type)} #{result_tmp} = #{default_val(result_type)};")

      first = true
      (node.conditions || []).each do |cond|
        next unless cond.is_a?(Prism::InNode)
        check = compile_pattern_check(cond.pattern, pred, pred_type)
        if first
          emit("if (#{check}) {")
          first = false
        else
          emit("else if (#{check}) {")
        end
        @indent += 1
        if cond.statements
          val = compile_block_expr_from_stmts(cond.statements)
          emit("#{result_tmp} = #{val};")
        end
        @indent -= 1
        emit("}")
      end
      if node.else_clause
        emit("else {")
        @indent += 1
        val = compile_block_expr_from_stmts(node.else_clause.statements)
        emit("#{result_tmp} = #{val};")
        @indent -= 1
        emit("}")
      end
      result_tmp
    end

    def compile_block_expr_from_stmts(body)
      return "0" unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      if stmts.length == 1
        compile_expr(stmts.first)
      else
        stmts[0..-2].each { |s| generate_stmt(s) }
        compile_expr(stmts.last)
      end
    end

    def compile_pattern_check(pattern, pred, pred_type)
      case pattern
      when Prism::ConstantReadNode
        cname = pattern.name.to_s
        if pred_type == Type::POLY
          @needs_poly = true
          case cname
          when "Integer" then "SP_IS_INT(#{pred})"
          when "String" then "SP_IS_STR(#{pred})"
          when "Float" then "SP_IS_DBL(#{pred})"
          when "NilClass" then "SP_IS_NIL(#{pred})"
          else "(0) /* unknown pattern #{cname} */"
          end
        else
          # Non-poly: simple type check
          case cname
          when "Integer" then "1" # if pred is already int
          when "String" then "1"
          else "0"
          end
        end
      when Prism::IntegerNode
        val = pattern.value.to_s
        if pred_type == Type::POLY
          @needs_poly = true
          "(SP_IS_INT(#{pred}) && sp_unbox_int(#{pred}) == #{val})"
        else
          "(#{pred} == #{val})"
        end
      when Prism::StringNode
        val = c_string_literal(pattern.unescaped)
        if pred_type == Type::POLY
          @needs_poly = true
          "(SP_IS_STR(#{pred}) && strcmp(sp_unbox_str(#{pred}), #{val}) == 0)"
        else
          "(strcmp(#{pred}, #{val}) == 0)"
        end
      when Prism::TrueNode
        if pred_type == Type::POLY
          "(SP_IS_BOOL(#{pred}) && sp_unbox_bool(#{pred}))"
        else
          "(#{pred})"
        end
      when Prism::FalseNode
        if pred_type == Type::POLY
          "(SP_IS_BOOL(#{pred}) && !sp_unbox_bool(#{pred}))"
        else
          "(!#{pred})"
        end
      when Prism::NilNode
        if pred_type == Type::POLY
          "SP_IS_NIL(#{pred})"
        else
          "(#{pred} == 0)"
        end
      when Prism::AlternationPatternNode
        left = compile_pattern_check(pattern.left, pred, pred_type)
        right = compile_pattern_check(pattern.right, pred, pred_type)
        "(#{left} || #{right})"
      else
        "(0) /* unsupported pattern #{pattern.class} */"
      end
    end

    def generate_case_stmt(node, return_type: nil)
      if node.predicate.nil?
        # case with no predicate (acts like if/elsif)
        generate_case_no_predicate(node, return_type)
        return
      end

      pred = compile_expr(node.predicate)
      pred_type = infer_type(node.predicate)
      temp_pred = "_cpred_#{next_temp}"

      if return_type
        res_var = "_cres_#{next_temp}"
        emit("#{c_type(pred_type)} #{temp_pred} = #{pred};")
        emit("#{c_type(return_type)} #{res_var} = #{default_val(return_type)};")
      else
        emit("#{c_type(pred_type)} #{temp_pred} = #{pred};")
      end

      first = true
      (node.conditions || []).each do |when_node|
        next unless when_node.is_a?(Prism::WhenNode)

        conditions = when_node.conditions.map { |c| compile_when_condition(c, temp_pred, pred_type) }
        cond_str = conditions.join(" || ")

        if first
          emit("if (#{cond_str}) {")
          first = false
        else
          emit("else if (#{cond_str}) {")
        end

        @indent += 1
        if return_type && when_node.statements
          stmts = when_node.statements.is_a?(Prism::StatementsNode) ? when_node.statements.body : [when_node.statements]
          stmts.each_with_index do |s, i|
            if i == stmts.length - 1
              compile_last_into(s, res_var)
            else
              generate_stmt(s)
            end
          end
        else
          generate_body_stmts(when_node.statements)
        end
        @indent -= 1
        emit("}")
      end

      if node.else_clause
        emit("else {")
        @indent += 1
        if return_type && node.else_clause.statements
          stmts = node.else_clause.statements.is_a?(Prism::StatementsNode) ? node.else_clause.statements.body : [node.else_clause.statements]
          stmts.each_with_index do |s, i|
            if i == stmts.length - 1
              compile_last_into(s, res_var)
            else
              generate_stmt(s)
            end
          end
        else
          generate_body_stmts(node.else_clause.statements)
        end
        @indent -= 1
        emit("}")
      end

      if return_type
        emit_gc_return(res_var)
      end
    end

    def generate_case_no_predicate(node, return_type)
      res_var = nil
      if return_type
        res_var = "_cres_#{next_temp}"
        emit("#{c_type(return_type)} #{res_var} = #{default_val(return_type)};")
      end

      first = true
      (node.conditions || []).each do |when_node|
        cond = compile_expr(when_node.conditions.first)
        if first
          emit("if (#{cond}) {")
          first = false
        else
          emit("else if (#{cond}) {")
        end
        @indent += 1
        if res_var && when_node.statements
          stmts = when_node.statements.is_a?(Prism::StatementsNode) ? when_node.statements.body : [when_node.statements]
          stmts[0..-2].each { |s| generate_stmt(s) } if stmts.length > 1
          compile_last_into(stmts.last, res_var)
        else
          generate_body_stmts(when_node.statements)
        end
        @indent -= 1
        emit("}")
      end
      if node.else_clause
        emit("else {")
        @indent += 1
        if res_var && node.else_clause.statements
          stmts = node.else_clause.statements.is_a?(Prism::StatementsNode) ? node.else_clause.statements.body : [node.else_clause.statements]
          stmts[0..-2].each { |s| generate_stmt(s) } if stmts.length > 1
          compile_last_into(stmts.last, res_var)
        else
          generate_body_stmts(node.else_clause.statements)
        end
        @indent -= 1
        emit("}")
      end
    end

    def compile_when_condition(cond, pred_var, pred_type)
      case cond
      when Prism::RangeNode
        @needs_range = true
        first = compile_expr(cond.left)
        last = compile_expr(cond.right)
        "(#{pred_var} >= #{first} && #{pred_var} <= #{last})"
      when Prism::IntegerNode
        "#{pred_var} == #{cond.value}"
      when Prism::StringNode
        if pred_type == Type::STRING
          "strcmp(#{pred_var}, #{compile_expr(cond)}) == 0"
        else
          "#{pred_var} == #{compile_expr(cond)}"
        end
      when Prism::SymbolNode
        if pred_type == Type::STRING
          "strcmp(#{pred_var}, \"#{cond.value}\") == 0"
        else
          "#{pred_var} == #{compile_expr(cond)}"
        end
      else
        "#{pred_var} == #{compile_expr(cond)}"
      end
    end

    def generate_begin_stmt(node)
      @needs_exception = true

      has_retry = body_has_retry?(node.rescue_clause)
      has_ensure = node.ensure_clause != nil

      old_retry_label = @current_retry_label
      if has_retry
        label = "_sp_retry_#{next_label}"
        @current_retry_label = label
        emit("#{label}: ;")
      end

      emit("/* begin/rescue */")
      emit("sp_exc_depth++;")
      emit("if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {")
      @indent += 1

      generate_body_stmts(node.statements)
      emit("sp_exc_depth--;")

      @indent -= 1
      emit("}")

      if node.rescue_clause
        emit("else {")
        @indent += 1
        emit("sp_exc_depth--;")

        rc = node.rescue_clause
        has_class_checks = rc.exceptions && !rc.exceptions.empty?

        if has_class_checks
          # Multiple rescue clauses with class checks
          first = true
          while rc
            if rc.exceptions && !rc.exceptions.empty?
              cls_names = rc.exceptions.map { |e|
                e.is_a?(Prism::ConstantReadNode) ? e.name.to_s : "RuntimeError"
              }
              cond = cls_names.map { |c| "sp_exc_is_a(\"#{c}\")" }.join(" || ")
              if first
                emit("if (#{cond}) {")
                first = false
              else
                emit("else if (#{cond}) {")
              end
            else
              # bare rescue
              emit("else {") unless first
              emit("{") if first
              first = false
            end
            @indent += 1
            if rc.reference
              ename = case rc.reference
                      when Prism::LocalVariableTargetNode then rc.reference.name.to_s
                      else "e"
                      end
              ensure_var_declared(ename, Type::STRING)
              emit("lv_#{ename} = sp_exc_message;")
            end
            generate_body_stmts(rc.statements)
            @indent -= 1
            emit("}")
            rc = rc.subsequent
          end
        else
          # Simple rescue (no class check)
          if rc.reference
            ename = case rc.reference
                    when Prism::LocalVariableTargetNode then rc.reference.name.to_s
                    else "e"
                    end
            ensure_var_declared(ename, Type::STRING)
            emit("lv_#{ename} = sp_exc_message;")
          end
          generate_body_stmts(rc.statements)
        end

        @indent -= 1
        emit("}")
      end

      if has_ensure
        generate_body_stmts(node.ensure_clause.statements)
      end

      @current_retry_label = old_retry_label
    end

    def body_has_retry?(node)
      return false unless node
      case node
      when Prism::RetryNode
        true
      when Prism::StatementsNode
        node.body.any? { |s| body_has_retry?(s) }
      when Prism::IfNode
        body_has_retry?(node.statements) || body_has_retry?(node.subsequent)
      when Prism::RescueNode
        body_has_retry?(node.statements)
      else
        if node.respond_to?(:statements)
          body_has_retry?(node.statements)
        else
          false
        end
      end
    end

    def generate_return(node)
      if node.arguments && !node.arguments.arguments.empty?
        val = compile_expr(node.arguments.arguments.first)
        emit_gc_return(val)
      else
        if @gc_restore_before_return
          emit("SP_GC_RESTORE();")
        end
        emit("return;")
      end
    end

    def generate_yield(node)
      args = node.arguments ? node.arguments.arguments : []
      if args.empty?
        emit("_block(_block_env, 0);")
      else
        val = compile_expr(args.first)
        emit("_block(_block_env, #{val});")
      end
    end

    def generate_rescue_modifier(node)
      @needs_exception = true
      # expr rescue fallback
      emit("/* begin/rescue */")
      emit("sp_exc_depth++;")
      emit("if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {")
      @indent += 1
      generate_stmt(node.expression)
      emit("sp_exc_depth--;")
      @indent -= 1
      emit("}")
      emit("else {")
      @indent += 1
      emit("sp_exc_depth--;")
      generate_stmt(node.rescue_expression)
      @indent -= 1
      emit("}")
    end

    # ---- Expression compilation ----
    def compile_expr(node)
      return "0" unless node
      case node
      when Prism::IntegerNode
        node.value.to_s
      when Prism::FloatNode
        # Use full precision, ensure decimal point for C float literal
        s = "%.16g" % node.value
        s = s + ".0" unless s.include?('.') || s.include?('e') || s.include?('E')
        s
      when Prism::StringNode
        c_string_literal(node.respond_to?(:unescaped) ? node.unescaped : node.content)
      when Prism::InterpolatedStringNode
        compile_interpolated_string(node)
      when Prism::SymbolNode
        c_string_literal(node.value)
      when Prism::TrueNode
        "TRUE"
      when Prism::FalseNode
        "FALSE"
      when Prism::NilNode
        "0"
      when Prism::SelfNode
        "self"
      when Prism::LocalVariableReadNode
        "lv_#{node.name}"
      when Prism::LocalVariableWriteNode
        name = node.name.to_s
        val = compile_expr(node.value)
        ensure_var_declared(name, infer_type(node.value))
        "(lv_#{name} = #{val})"
      when Prism::LocalVariableOperatorWriteNode
        name = node.name.to_s
        op = node.binary_operator.to_s
        val = compile_expr(node.value)
        var_info = lookup_var(name)
        var_type = var_info ? var_info.type : Type::UNKNOWN
        val_type = infer_type(node.value)
        if op == "/" && var_type == Type::INTEGER && val_type == Type::INTEGER
          "(lv_#{name} = sp_idiv(lv_#{name}, #{val}))"
        elsif op == "%" && var_type == Type::INTEGER && val_type == Type::INTEGER
          "(lv_#{name} = sp_imod(lv_#{name}, #{val}))"
        else
          "(lv_#{name} #{op}= #{val})"
        end
      when Prism::InstanceVariableReadNode
        compile_ivar_read(node)
      when Prism::InstanceVariableWriteNode
        ivar = node.name.to_s.delete_prefix("@")
        val = compile_expr(node.value)
        if @current_module
          "(sp_#{@current_module}_#{ivar} = #{val})"
        elsif @current_class
          ci = @classes[@current_class]
          needs_ptr = class_needs_gc?(ci)
          accessor = needs_ptr ? "self->#{ivar}" : "self.#{ivar}"
          "(#{accessor} = #{val})"
        else
          val
        end
      when Prism::ConstantReadNode
        compile_constant_read(node)
      when Prism::ConstantPathNode
        compile_constant_path(node)
      when Prism::GlobalVariableReadNode
        compile_global_read(node)
      when Prism::ArrayNode
        compile_array_literal(node)
      when Prism::HashNode
        compile_hash_literal(node)
      when Prism::RangeNode
        compile_range(node)
      when Prism::CallNode
        compile_call(node)
      when Prism::IfNode
        compile_if_expr(node)
      when Prism::UnlessNode
        compile_unless_expr(node)
      when Prism::ParenthesesNode
        if node.body
          inner = compile_expr(node.body.is_a?(Prism::StatementsNode) && node.body.body.length == 1 ? node.body.body.first : node.body)
          "(#{inner})"
        else
          "0"
        end
      when Prism::StatementsNode
        if node.body.length == 1
          compile_expr(node.body.first)
        else
          # Multiple statements - compile last
          node.body[0..-2].each { |s| generate_stmt(s) }
          compile_expr(node.body.last)
        end
      when Prism::BeginNode
        if node.rescue_clause
          compile_rescue_expr(node)
        elsif node.statements
          compile_expr(node.statements)
        else
          "0"
        end
      when Prism::ReturnNode
        if node.arguments && !node.arguments.arguments.empty?
          val = compile_expr(node.arguments.arguments.first)
          "return #{val}"
        else
          "return"
        end
      when Prism::YieldNode
        args = node.arguments ? node.arguments.arguments : []
        if args.empty?
          "_block(_block_env, 0)"
        else
          "_block(_block_env, #{compile_expr(args.first)})"
        end
      when Prism::DefinedNode
        # defined?(x) -> check if variable is known
        expr = node.value
        case expr
        when Prism::LocalVariableReadNode
          v = lookup_var(expr.name.to_s)
          v ? '"local-variable"' : "0"
        else
          '"expression"'
        end
      when Prism::SourceLineNode
        node.respond_to?(:location) ? node.location.start_line.to_s : "0"
      when Prism::RescueModifierNode
        compile_rescue_modifier_expr(node)
      when Prism::CaseNode
        compile_case_expr(node)
      when Prism::CaseMatchNode
        compile_case_match_expr(node)
      when Prism::SuperNode, Prism::ForwardingSuperNode
        compile_super(node)
      when Prism::MultiWriteNode
        compile_multi_write_expr(node)
      when Prism::LambdaNode
        "0 /* lambda not supported */"
      when Prism::XStringNode
        # Backtick execution: `cmd`
        @needs_system = true
        cmd = c_string_literal(node.respond_to?(:unescaped) ? node.unescaped : node.content)
        "sp_backtick(#{cmd})"
      when Prism::SplatNode
        if node.expression
          compile_expr(node.expression)
        else
          "0"
        end
      when Prism::AndNode
        left = compile_expr(node.left)
        right = compile_expr(node.right)
        "(#{left} && #{right})"
      when Prism::OrNode
        left = compile_expr(node.left)
        right = compile_expr(node.right)
        "(#{left} || #{right})"
      when Prism::RegularExpressionNode
        # Register the pattern and return the compiled regex variable
        pattern = node.unescaped
        c_var = register_regexp(pattern)
        c_var
      when Prism::NumberedReferenceReadNode
        @needs_regexp = true
        "sp_re_group(#{node.number})"
      when Prism::MatchWriteNode
        # if str =~ /pattern/ form (MatchWriteNode wraps a CallNode)
        compile_expr(node.call)
      else
        "0 /* unsupported: #{node.class.name.split('::').last} */"
      end
    end

    def compile_expr_static(node)
      compile_expr(node)
    end

    def compile_expr_in_method(node, ci, needs_gc_alloc)
      compile_expr(node)
    end

    def compile_ivar_read(node)
      ivar = node.name.to_s.delete_prefix("@")
      if @current_module
        "sp_#{@current_module}_#{ivar}"
      elsif @current_class
        ci = @classes[@current_class]
        needs_ptr = class_needs_gc?(ci)
        needs_ptr ? "self->#{ivar}" : "self.#{ivar}"
      else
        ivar
      end
    end

    def compile_constant_read(node)
      name = node.name.to_s
      if @constants[name]
        "cv_#{name}"
      elsif @current_module && @module_constants["#{@current_module}::#{name}"]
        @module_constants["#{@current_module}::#{name}"][:c_name]
      elsif name == "ARGV"
        "sp_argv"
      elsif name == "STDOUT"
        "stdout"
      elsif name == "STDERR"
        "stderr"
      elsif name == "Math"
        "0 /* Math module */"
      else
        # Could be a class name
        if @classes[name]
          "/* #{name} */"
        else
          "cv_#{name}"
        end
      end
    end

    def compile_constant_path(node)
      path = constant_path_str(node)
      if @module_constants[path]
        @module_constants[path][:c_name]
      elsif path == "Math::PI"
        "M_PI"
      elsif path == "Float::INFINITY"
        "(1.0/0.0)"
      elsif path == "Float::NAN"
        "(0.0/0.0)"
      else
        # Try class::CONST pattern
        parts = path.split("::")
        if parts.length == 2 && @classes[parts[0]]
          "sp_#{parts[0]}_#{parts[1]}"
        else
          "0 /* #{path} */"
        end
      end
    end

    def constant_path_str(node)
      case node
      when Prism::ConstantPathNode
        parent = node.parent ? constant_path_str(node.parent) : ""
        child = node.name.to_s
        parent.empty? ? child : "#{parent}::#{child}"
      when Prism::ConstantReadNode
        node.name.to_s
      else
        node.to_s
      end
    end

    def compile_global_read(node)
      gname = node.name.to_s
      case gname
      when "$stdout" then "stdout"
      when "$stderr" then "stderr"
      when "$stdin" then "stdin"
      when "$0" then "sp_program_name"
      when "$PROGRAM_NAME" then "sp_program_name"
      else
        "0 /* #{gname} */"
      end
    end

    def compile_array_literal(node)
      if node.elements.empty?
        @needs_int_array = true
        @needs_gc = true
        tmp = "_ary_#{next_temp}"
        emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
        return tmp
      end

      arr_type = infer_type(node)
      if arr_type == Type::POLY_ARRAY
        # Heterogeneous array: store boxed sp_RbValue in IntArray (same size)
        @needs_int_array = true
        @needs_poly = true
        @needs_gc = true
        tmp = "_ary_#{next_temp}"
        emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
        node.elements.each do |elem|
          val = compile_expr(elem)
          et = infer_type(elem)
          boxed = box_value(val, et)
          emit("sp_IntArray_push(#{tmp}, (mrb_int)#{boxed});")
        end
        return tmp
      elsif arr_type == Type::STR_ARRAY
        @needs_str_array = true
        tmp = "_ary_#{next_temp}"
        emit("sp_StrArray *#{tmp} = sp_StrArray_new();")
        node.elements.each do |elem|
          val = compile_expr(elem)
          emit("sp_StrArray_push(#{tmp}, #{val});")
        end
        return tmp
      else
        @needs_int_array = true
        @needs_gc = true
        tmp = "_ary_#{next_temp}"
        emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
        node.elements.each do |elem|
          val = compile_expr(elem)
          emit("sp_IntArray_push(#{tmp}, #{val});")
        end
        return tmp
      end
    end

    def compile_hash_literal(node)
      if node.elements.empty?
        @needs_str_int_hash = true
        @needs_gc = true
        return "sp_StrIntHash_new()"
      end

      # Determine hash type from value types
      ht = infer_type(node)
      first = node.elements.first
      if first.is_a?(Prism::AssocNode)
        if ht == Type::POLY_HASH
          @needs_poly_hash = true
          @needs_poly = true
          @needs_gc = true
          tmp = "_hsh_#{next_temp}"
          emit("sp_PolyHash *#{tmp} = sp_PolyHash_new();")
          node.elements.each do |elem|
            next unless elem.is_a?(Prism::AssocNode)
            k = compile_hash_key(elem.key)
            v = compile_expr(elem.value)
            vt = infer_type(elem.value)
            emit("sp_PolyHash_set(#{tmp}, #{k}, #{box_value(v, vt)});")
          end
          return tmp
        elsif ht == Type::STR_HASH
          @needs_str_str_hash = true
          tmp = "_hsh_#{next_temp}"
          emit("sp_RbHash *#{tmp} = sp_RbHash_new();")
          node.elements.each do |elem|
            next unless elem.is_a?(Prism::AssocNode)
            k = compile_hash_key(elem.key)
            v = compile_expr(elem.value)
            emit("sp_RbHash_set(#{tmp}, #{k}, #{v});")
          end
          return tmp
        else
          @needs_str_int_hash = true
          @needs_gc = true
          tmp = "_hsh_#{next_temp}"
          emit("sp_StrIntHash *#{tmp} = sp_StrIntHash_new();")
          node.elements.each do |elem|
            next unless elem.is_a?(Prism::AssocNode)
            k = compile_hash_key(elem.key)
            v = compile_expr(elem.value)
            emit("sp_StrIntHash_set(#{tmp}, #{k}, #{v});")
          end
          return tmp
        end
      end

      "0"
    end

    def compile_hash_key(node)
      case node
      when Prism::SymbolNode
        c_string_literal(node.value)
      when Prism::StringNode
        c_string_literal(node.respond_to?(:unescaped) ? node.unescaped : node.content)
      else
        compile_expr(node)
      end
    end

    def compile_range(node)
      @needs_range = true
      first = compile_expr(node.left)
      last = compile_expr(node.right)
      "sp_Range_new(#{first}, #{last})"
    end

    def compile_interpolated_string(node)
      @string_helpers_needed << :str_concat
      @string_helpers_needed << :int_to_s
      parts = node.parts.map do |part|
        case part
        when Prism::StringNode
          c_string_literal(part.respond_to?(:unescaped) ? part.unescaped : part.content)
        when Prism::EmbeddedStatementsNode
          if part.statements && part.statements.body.length > 0
            expr = part.statements.body.first
            type = infer_type(expr)
            val = compile_expr(expr)
            # Check if expr is a poly variable
            if expr.is_a?(Prism::LocalVariableReadNode)
              v = lookup_var(expr.name.to_s)
              type = Type::POLY if v && v.type == Type::POLY
            end
            case type
            when Type::POLY
              @needs_poly = true
              "sp_poly_to_s(#{val})"
            when Type::INTEGER
              "sp_int_to_s(#{val})"
            when Type::FLOAT
              @string_helpers_needed << :float_to_s
              "sp_float_to_s(#{val})"
            when Type::STRING
              val
            when Type::BOOLEAN
              "(#{val} ? \"true\" : \"false\")"
            else
              "sp_int_to_s(#{val})"
            end
          else
            '""'
          end
        else
          '""'
        end
      end

      if parts.length == 1
        parts[0]
      else
        # Chain sp_str_concat
        result = parts[0]
        parts[1..].each do |p|
          result = "sp_str_concat(#{result}, #{p})"
        end
        result
      end
    end

    def compile_if_expr(node)
      then_stmts = node.statements.is_a?(Prism::StatementsNode) ? node.statements.body : [node.statements] if node.statements
      then_stmts ||= []
      else_body = case node.subsequent
                  when Prism::ElseNode then node.subsequent.statements
                  when Prism::IfNode then node.subsequent
                  else node.subsequent
                  end if node.subsequent
      else_stmts = if else_body.is_a?(Prism::StatementsNode)
                     else_body.body
                   elsif else_body
                     [else_body]
                   else
                     []
                   end
      then_last = then_stmts.last
      else_last = else_stmts.last
      has_return = then_last.is_a?(Prism::ReturnNode) || else_last.is_a?(Prism::ReturnNode)
      multi_stmt = then_stmts.length > 1 || else_stmts.length > 1
      if has_return || multi_stmt
        # Emit as if/else statement with result variable
        result_type = infer_type(node)
        res_var = "_cres_#{next_temp}"
        cond = compile_expr(node.predicate)
        emit("#{c_type(result_type)} #{res_var} = #{default_val(result_type)};")
        emit("if (#{cond}) {")
        @indent += 1
        then_stmts.each_with_index do |s, i|
          if i == then_stmts.length - 1 && !s.is_a?(Prism::ReturnNode)
            emit("#{res_var} = #{compile_expr(s)};")
          else
            generate_stmt(s)
          end
        end
        @indent -= 1
        emit("}")
        if else_stmts.any?
          emit("else {")
          @indent += 1
          else_stmts.each_with_index do |s, i|
            if i == else_stmts.length - 1 && !s.is_a?(Prism::ReturnNode)
              emit("#{res_var} = #{compile_expr(s)};")
            else
              generate_stmt(s)
            end
          end
          @indent -= 1
          emit("}")
        end
        return res_var
      end
      cond = compile_expr(node.predicate)
      then_val = then_last ? compile_expr(then_last) : "0"
      else_val = else_last ? compile_expr(else_last) : "0"
      "(#{cond} ? #{then_val} : #{else_val})"
    end

    def compile_unless_expr(node)
      cond = compile_expr(node.predicate)
      then_val = node.statements ? compile_expr(node.statements.is_a?(Prism::StatementsNode) ? node.statements.body.last : node.statements) : "0"
      else_val = node.else_clause ? compile_expr(node.else_clause.statements) : "0"
      "(!(#{cond}) ? #{then_val} : #{else_val})"
    end

    def compile_rescue_expr(node)
      # begin/rescue as expression
      @needs_exception = true
      "0 /* rescue expr */"
    end

    def compile_rescue_modifier_expr(node)
      @needs_exception = true
      fallback = compile_expr(node.rescue_expression)
      tmp = "_resc_#{next_temp}"
      type = infer_type(node.rescue_expression)
      emit("#{c_type(type)} #{tmp};")
      emit("sp_exc_depth++;")
      emit("if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {")
      @indent += 1
      # Generate the expression (which may include raise) inside the try block
      if node.expression.is_a?(Prism::CallNode) && node.expression.name.to_s == "raise"
        generate_raise(node.expression)
        emit("#{tmp} = 0;")
      else
        val = compile_expr(node.expression)
        emit("#{tmp} = #{val};")
      end
      emit("sp_exc_depth--;")
      @indent -= 1
      emit("}")
      emit("else {")
      emit("  sp_exc_depth--;")
      emit("  #{tmp} = #{fallback};")
      emit("}")
      tmp
    end

    def compile_case_expr(node)
      # Compile case as expression using temp var
      if node.predicate
        pred = compile_expr(node.predicate)
        pred_type = infer_type(node.predicate)
        temp_pred = "_cpred_#{next_temp}"
        result_type = infer_case_type(node)
        res_var = "_cres_#{next_temp}"

        emit("#{c_type(pred_type)} #{temp_pred} = #{pred};")
        emit("#{c_type(result_type)} #{res_var} = #{default_val(result_type)};")

        first = true
        (node.conditions || []).each do |when_node|
          next unless when_node.is_a?(Prism::WhenNode)
          conditions = when_node.conditions.map { |c| compile_when_condition(c, temp_pred, pred_type) }
          cond_str = conditions.join(" || ")

          if first
            emit("if (#{cond_str}) {")
            first = false
          else
            emit("else if (#{cond_str}) {")
          end

          @indent += 1
          if when_node.statements
            stmts = when_node.statements.is_a?(Prism::StatementsNode) ? when_node.statements.body : [when_node.statements]
            stmts.each_with_index do |s, i|
              if i == stmts.length - 1
                emit("#{res_var} = #{compile_expr(s)};")
              else
                generate_stmt(s)
              end
            end
          end
          @indent -= 1
          emit("}")
        end

        if node.else_clause
          emit("else {")
          @indent += 1
          if node.else_clause.statements
            stmts = node.else_clause.statements.is_a?(Prism::StatementsNode) ? node.else_clause.statements.body : [node.else_clause.statements]
            stmts.each_with_index do |s, i|
              if i == stmts.length - 1
                emit("#{res_var} = #{compile_expr(s)};")
              else
                generate_stmt(s)
              end
            end
          end
          @indent -= 1
          emit("}")
        end

        res_var
      else
        # Bare case as expression
        result_type = infer_case_type(node)
        res_var = "_cres_#{next_temp}"
        emit("#{c_type(result_type)} #{res_var} = #{default_val(result_type)};")

        first = true
        (node.conditions || []).each do |when_node|
          cond = compile_expr(when_node.conditions.first)
          if first
            emit("if (#{cond}) {")
            first = false
          else
            emit("else if (#{cond}) {")
          end
          @indent += 1
          if when_node.statements
            stmts = when_node.statements.is_a?(Prism::StatementsNode) ? when_node.statements.body : [when_node.statements]
            stmts[0..-2].each { |s| generate_stmt(s) } if stmts.length > 1
            emit("#{res_var} = #{compile_expr(stmts.last)};")
          end
          @indent -= 1
          emit("}")
        end
        if node.else_clause && node.else_clause.statements
          emit("else {")
          @indent += 1
          stmts = node.else_clause.statements.is_a?(Prism::StatementsNode) ? node.else_clause.statements.body : [node.else_clause.statements]
          stmts[0..-2].each { |s| generate_stmt(s) } if stmts.length > 1
          emit("#{res_var} = #{compile_expr(stmts.last)};")
          @indent -= 1
          emit("}")
        end
        res_var
      end
    end

    def compile_super(node)
      return "0" unless @current_class
      ci = @classes[@current_class]
      return "0" unless ci.parent

      args = node.respond_to?(:arguments) && node.arguments ? node.arguments.arguments.map { |a| compile_expr(a) } : []
      "sp_#{ci.parent}_initialize((sp_#{ci.parent} *)self, #{args.join(', ')})"
    end

    def compile_multi_write_expr(node)
      # handled at statement level
      generate_multi_write(node)
      "0"
    end

    # ---- Call expression compilation ----
    def compile_call(node)
      mname = node.name.to_s
      recv = node.receiver
      args = call_args(node)

      # Check for modifier if/unless on expressions
      if recv.nil? && mname == "puts"
        # This shouldn't be called as expression, but handle it
        generate_puts(node)
        return "0"
      end

      if recv
        return compile_method_call(node, recv, mname, args)
      end

      # No receiver - top-level call
      compile_toplevel_call(node, mname, args)
    end

    def compile_toplevel_call(node, mname, args)
      # Check if inside an open class method - treat bare calls as self method calls
      if @current_open_class_type && !@current_class
        # Create a synthetic SelfNode-like receiver
        compiled_args = args.map { |a| compile_expr(a) }
        type_name = @current_open_class_type
        # Check if it's another open class method
        if @open_class_methods && @open_class_methods[type_name] && @open_class_methods[type_name][mname]
          cmname = sanitize_method_name(mname)
          return "sp_#{type_name}_#{cmname}(self#{compiled_args.empty? ? '' : ', ' + compiled_args.join(', ')})"
        end
        # Otherwise, treat as a built-in method on self
        recv_type = case type_name
                    when "Integer" then Type::INTEGER
                    when "Float" then Type::FLOAT
                    when "String" then Type::STRING
                    when "Boolean" then Type::BOOLEAN
                    else Type::UNKNOWN
                    end
        # Use compile_method_call logic with self as receiver
        mstr_recv = recv_type == Type::MUTABLE_STRING ? "sp_String_cstr(self)" : "self"
        case mname
        when "upcase"
          @string_helpers_needed << :str_upcase
          return "sp_str_upcase(self)"
        when "downcase"
          @string_helpers_needed << :str_downcase
          return "sp_str_downcase(self)"
        when "to_s"
          return "self" if recv_type == Type::STRING
          return "sp_int_to_s(self)" if recv_type == Type::INTEGER
          return "sp_float_to_s(self)" if recv_type == Type::FLOAT
        end
      end

      # Check implicit self method calls in class context
      if @current_class
        ci = @classes[@current_class]
        actual_class = find_method_class(@current_class, mname)
        if actual_class
          needs_ptr = class_needs_gc?(ci)
          compiled_args = args.map { |a| compile_expr(a) }
          cmname = sanitize_method_name(mname)
          if actual_class == @current_class
            return "sp_#{actual_class}_#{cmname}(#{(['self'] + compiled_args).join(', ')})"
          else
            if needs_ptr
              return "sp_#{actual_class}_#{cmname}(#{(["(sp_#{actual_class} *)self"] + compiled_args).join(', ')})"
            else
              return "sp_#{actual_class}_#{cmname}(#{(['self'] + compiled_args).join(', ')})"
            end
          end
        end
        # Check ivar access (bare name matches attr reader)
        if ci.attrs[:reader].include?(mname) || ci.attrs[:accessor].include?(mname)
          ptr = class_needs_gc?(ci)
          return ptr ? "self->#{mname}" : "self.#{mname}"
        end
      end

      case mname
      when "__method__"
        return @current_method ? c_string_literal(@current_method.name) : '""'
      when "puts", "print", "p", "printf"
        generate_call_stmt(node)
        return "0"
      when "raise"
        generate_raise(node)
        return "0"
      when "format", "sprintf"
        if args.length >= 1
          fmt = compile_expr(args[0])
          rest = args[1..].map { |a| compile_expr(a) }.join(", ")
          @string_helpers_needed << :str_concat
          if rest.empty?
            return "#{fmt}"
          end
          # Build sprintf call that returns string
          tmp = "_fmt_#{next_temp}"
          emit("char #{tmp}[256];")
          emit("snprintf(#{tmp}, sizeof(#{tmp}), #{fmt}, #{rest});")
          # Return as heap string
          ret = "_fmts_#{next_temp}"
          emit("const char *#{ret} = (const char *)malloc(strlen(#{tmp}) + 1);")
          emit("strcpy((char *)#{ret}, #{tmp});")
          return ret
        end
        return '""'
      when "Integer"
        if args.length > 0
          arg_node = args[0]
          # Handle Integer(ARGV[n] || default) pattern
          if arg_node.is_a?(Prism::OrNode)
            left = arg_node.left
            right = arg_node.right
            if left.is_a?(Prism::CallNode) && left.name.to_s == "[]" &&
               left.receiver.is_a?(Prism::ConstantReadNode) && left.receiver.name.to_s == "ARGV"
              idx = compile_expr(left.arguments.arguments[0])
              default_val = compile_expr(right)
              return "(sp_argv.len > #{idx} ? atol(sp_argv.data[#{idx}]) : #{default_val})"
            end
          end
          arg = compile_expr(arg_node)
          arg_type = infer_type(arg_node)
          if arg_type == Type::STRING
            return "atol(#{arg})"
          else
            return "((mrb_int)(#{arg}))"
          end
        end
        return "0"
      when "Float"
        if args.length > 0
          arg = compile_expr(args[0])
          return "((mrb_float)(#{arg}))"
        end
        return "0.0"
      when "rand"
        return "((mrb_int)(rand() % 100))"
      when "sleep"
        val = args.empty? ? "0" : compile_expr(args[0])
        return "sleep(#{val})"
      when "exit"
        val = args.empty? ? "0" : compile_expr(args[0])
        emit("exit(#{val});")
        return "0"
      when "gets"
        return 'NULL /* gets */'
      when "method"
        # method(:name) returns a method object - limited support
        return "0 /* method ref */"
      when "defined?"
        return '"expression"'
      when "block_given?"
        return "(_block != NULL)"
      when "loop"
        generate_loop(node)
        return "0"
      when "system"
        @needs_system = true
        if args.length > 0
          arg = compile_expr(args[0])
          return "sp_system(#{arg})"
        end
        return "0"
      when "trap"
        # Register signal handler - no-op for AOT
        emit("/* trap: no-op */")
        return "0"
      when "putc"
        if args.length > 0
          val = compile_expr(args[0])
          arg_type = infer_type(args[0])
          if arg_type == Type::STRING
            emit("fputs(#{val}, stdout);")
          else
            emit("putchar((char)#{val});")
          end
        end
        return "0"
      when "srand"
        if args.length > 0
          arg = compile_expr(args[0])
          emit("srand(#{arg});")
        else
          emit("srand(0);")
        end
        return "0"
      when "rand"
        if args.length > 0
          arg = compile_expr(args[0])
          return "(rand() % #{arg})"
        end
        return "(rand())"
      when "exit"
        if args.length > 0
          arg = compile_expr(args[0])
          emit("exit(#{arg});")
        else
          emit("exit(0);")
        end
        return "0"
      when "catch"
        return compile_catch(node, args)
      when "throw"
        return compile_throw(node, args)
      when "proc"
        if node.block
          return compile_proc_creation(node.block)
        end
        return "NULL"
      end

      # Proc.new { ... }
      if mname == "new" && recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Proc" && node.block
        return compile_proc_creation(node.block)
      end

      # User-defined method
      if @methods[mname]
        mi = @methods[mname]

        # Handle keyword arguments
        if mi.has_kwargs && args.any? { |a| a.is_a?(Prism::KeywordHashNode) }
          compiled_args = compile_kwargs_call(mi, args)
        elsif mi.has_rest
          # Rest args: pack into IntArray
          @needs_int_array = true
          @needs_gc = true
          rest_tmp = "_rest_#{next_temp}"
          emit("sp_IntArray *#{rest_tmp} = sp_IntArray_new();")
          args.each do |a|
            val = compile_expr(a)
            emit("sp_IntArray_push(#{rest_tmp}, #{val});")
          end
          compiled_args = [rest_tmp]
        else
          compiled_args = args.each_with_index.map { |a, i|
            val = compile_expr(a)
            # Handle type coercion if needed
            if i < mi.params.length && mi.params[i].type == Type::POLY
              # Box value for polymorphic param
              @needs_poly = true
              arg_type = infer_type(a)
              box_value(val, arg_type)
            elsif i < mi.params.length && mi.params[i].type == Type::STRING && infer_type(a) == Type::INTEGER
              @string_helpers_needed << :int_to_s
              "sp_int_to_s(#{val})"
            else
              val
            end
          }
        end

        # Fill in default args
        while compiled_args.length < mi.params.length
          p = mi.params[compiled_args.length]
          if p.default_node
            compiled_args << compile_expr(p.default_node)
          else
            compiled_args << default_val(p.type)
          end
        end

        block_param_name = mi.instance_variable_get(:@block_param_name)
        if block_param_name
          # &block parameter: wrap block in sp_Proc
          @needs_proc = true
          @needs_block_fn = true
          if node.block
            blk_id = next_block_id
            blk_fn = "_blk_#{blk_id}"
            blk_env_type = "_blk_#{blk_id}_env"
            params_b = block_params(node.block)
            bparam = params_b[0] || "_x"
            # Emit block function
            @block_defs << "typedef struct { int _dummy; } #{blk_env_type};"
            @block_defs << "static mrb_int #{blk_fn}(void *_env, mrb_int _arg) {"
            @block_defs << "    #{blk_env_type} *_e = (#{blk_env_type} *)_env;"
            @block_defs << "    mrb_int lv_#{bparam} = _arg;"
            # Compile block body
            old_main = @in_main
            old_func = @func_bodies
            @in_main = false
            @func_bodies = []
            push_scope
            declare_var(bparam, Type::INTEGER, c_name: "lv_#{bparam}")
            blk_val = compile_block_expr(node.block)
            pop_scope
            @block_defs.concat(@func_bodies.map { |l| "  " + l.strip })
            @func_bodies = old_func
            @in_main = old_main
            @block_defs << "  return #{blk_val};"
            @block_defs << "    return 0;"
            @block_defs << "}"
            @block_defs << ""
            # Create sp_Proc on stack and pass its address
            proc_var = "_bp_#{blk_id}"
            emit("sp_Proc #{proc_var} = { (sp_block_fn)#{blk_fn}, NULL };")
            compiled_args << "&#{proc_var}"
          else
            compiled_args << "NULL"
          end
        elsif mi.has_yield && node.block
          # Need to create block struct and function
          @needs_block_fn = true
          block_code = compile_block_call(node.block, mi, mname, compiled_args)
          return block_code
        elsif mi.has_yield && !node.block
          # No block passed
          compiled_args << "NULL"
          compiled_args << "NULL"
        end

        return "sp_#{sanitize_method_name(mname)}(#{compiled_args.join(', ')})"
      end

      # Could be a method(:name).call(args) pattern
      if mname == "call"
        # Not directly supported
        return "0"
      end

      # Unknown method
      "0 /* unknown: #{mname} */"
    end

    def compile_kwargs_call(mi, args)
      # Extract keyword arguments from call args and map to param order
      kw_values = {}
      positional = []

      args.each do |a|
        if a.is_a?(Prism::KeywordHashNode)
          a.elements.each do |assoc|
            if assoc.is_a?(Prism::AssocNode) && assoc.key.is_a?(Prism::SymbolNode)
              key = assoc.key.value
              kw_values[key] = compile_expr(assoc.value)
            end
          end
        else
          positional << compile_expr(a)
        end
      end

      # Build args in param order
      compiled_args = []
      mi.params.each_with_index do |p, i|
        if i < positional.length
          compiled_args << positional[i]
        elsif kw_values[p.name]
          compiled_args << kw_values[p.name]
        elsif p.default_node
          compiled_args << compile_expr(p.default_node)
        else
          compiled_args << default_val(p.type)
        end
      end
      compiled_args
    end

    def compile_method_call(node, recv, mname, args)
      recv_type = infer_type(recv)
      recv_code = compile_expr(recv)

      # Safe navigation operator
      is_safe_nav = node.respond_to?(:call_operator) && node.call_operator == "&."

      # Check if receiver is POLY and a dispatch function exists for this method
      if recv_type == Type::POLY && @dispatch_methods && @dispatch_methods[mname]
        cmname = sanitize_method_name(mname)
        return "sp_dispatch_#{cmname}(#{recv_code})"
      end

      # Check if receiver is a class instance with this method defined (including operators)
      # This handles operator methods like <, >, ==, <=> on class instances
      if recv_type.is_a?(String) && @classes[recv_type]
        ci = @classes[recv_type]
        needs_ptr = class_needs_gc?(ci)
        # Prioritize attr reader/writer over method calls for direct field access
        if ci.attrs[:reader].include?(mname) || ci.attrs[:accessor].include?(mname)
          return needs_ptr ? "#{recv_code}->#{mname}" : "#{recv_code}.#{mname}"
        end
        if mname.end_with?("=") && (ci.attrs[:writer].include?(mname.chomp("=")) || ci.attrs[:accessor].include?(mname.chomp("=")))
          field = mname.chomp("=")
          val = compile_expr(args[0])
          return needs_ptr ? "(#{recv_code}->#{field} = #{val})" : "(#{recv_code}.#{field} = #{val})"
        end
        actual = find_method_class(recv_type, mname)
        if actual && @classes[actual].methods[mname]
          target_mi = @classes[actual].methods[mname]
          compiled_args = args.map { |a| compile_expr(a) }
          needs_ptr = class_needs_gc?(ci)
          cmname = sanitize_method_name(mname)
          if actual != recv_type && needs_ptr
            param_list = ["(sp_#{actual} *)#{recv_code}"] + compiled_args
          else
            param_list = [recv_code] + compiled_args
          end

          # Handle block/yield
          if target_mi.has_yield && node.block
            @needs_block_fn = true
            block_code = compile_block_call_for_class(node.block, target_mi, actual, cmname, param_list)
            return block_code
          elsif target_mi.has_yield && !node.block
            param_list << "NULL"
            param_list << "NULL"
          end

          return "sp_#{actual}_#{cmname}(#{param_list.join(', ')})"
        end
        # Check attr accessors when recv_type is a known class
        needs_ptr = class_needs_gc?(ci)
        if ci.attrs[:reader].include?(mname) || ci.attrs[:accessor].include?(mname)
          return needs_ptr ? "#{recv_code}->#{mname}" : "#{recv_code}.#{mname}"
        end
        if mname.end_with?("=") && (ci.attrs[:writer].include?(mname.chomp("=")) || ci.attrs[:accessor].include?(mname.chomp("=")))
          field = mname.chomp("=")
          val = compile_expr(args[0])
          return needs_ptr ? "(#{recv_code}->#{field} = #{val})" : "(#{recv_code}.#{field} = #{val})"
        end
      end

      # Check open class methods for built-in types
      if @open_class_methods
        type_name = case recv_type
                    when Type::INTEGER then "Integer"
                    when Type::FLOAT then "Float"
                    when Type::STRING then "String"
                    when Type::BOOLEAN then "Boolean"
                    else nil
                    end
        if type_name && @open_class_methods[type_name] && @open_class_methods[type_name][mname]
          compiled_args = args.map { |a| compile_expr(a) }
          cmname = sanitize_method_name(mname)
          return "sp_#{type_name}_#{cmname}(#{recv_code}#{compiled_args.empty? ? '' : ', ' + compiled_args.join(', ')})"
        end
      end

      # Auto-convert mutable strings to const char * for string methods
      # This variable used for methods that expect const char *
      mstr_recv = recv_type == Type::MUTABLE_STRING ? "sp_String_cstr(#{recv_code})" : recv_code

      # ---- $stderr.puts ----
      if recv.is_a?(Prism::GlobalVariableReadNode) && recv.name.to_s == "$stderr" && mname == "puts"
        args.each do |arg|
          val = compile_expr(arg)
          type = infer_type(arg)
          if type == Type::STRING
            emit("fprintf(stderr, \"%s\\n\", #{val});")
          else
            emit("fprintf(stderr, \"%lld\\n\", (long long)#{val});")
          end
        end
        return "0"
      end

      # ---- File object methods ----
      if recv_type == Type::FILE_OBJ
        @needs_file = true
        case mname
        when "puts"
          args.each do |arg|
            val = compile_expr(arg)
            emit("sp_File_puts(#{recv_code}, #{val});")
          end
          return "0"
        when "print", "write"
          args.each do |arg|
            val = compile_expr(arg)
            emit("sp_File_print(#{recv_code}, #{val});")
          end
          return "0"
        when "each_line"
          if node.block
            block = node.block
            params = block_params(block)
            line_var = params[0] || "line"
            buf_var = "_buf_#{next_temp}"
            len_var = "_len_#{buf_var}"
            emit("{ char #{buf_var}[4096];")
            emit("while (fgets(#{buf_var}, sizeof(#{buf_var}), #{recv_code}->fp)) {")
            @indent += 1
            emit("size_t #{len_var} = strlen(#{buf_var});")
            emit("if (#{len_var} > 0 && #{buf_var}[#{len_var}-1] == '\\n') #{buf_var}[#{len_var}-1] = '\\0';")
            emit("const char *lv_#{line_var} = #{buf_var};")
            push_scope
            declare_var(line_var, Type::STRING, c_name: "lv_#{line_var}")
            generate_block_body(block)
            pop_scope
            @indent -= 1
            emit("}")
            emit("}")
          end
          return "0"
        when "close"
          emit("sp_File_close(#{recv_code});")
          return "0"
        when "read"
          return "sp_File_read_all(#{recv_code})"
        end
      end

      # ---- StringIO methods ----
      if recv_type == Type::STRINGIO
        @needs_stringio = true
        case mname
        when "puts"
          if args.empty?
            emit("sp_StringIO_puts_empty(#{recv_code});")
          else
            args.each do |arg|
              val = compile_expr(arg)
              emit("sp_StringIO_puts(#{recv_code}, #{val});")
            end
          end
          return "0"
        when "print"
          args.each do |arg|
            val = compile_expr(arg)
            emit("sp_StringIO_print(#{recv_code}, #{val});")
          end
          return "0"
        when "write"
          val = compile_expr(args[0])
          return "sp_StringIO_write(#{recv_code}, #{val})"
        when "putc"
          val = compile_expr(args[0])
          return "sp_StringIO_putc(#{recv_code}, #{val})"
        when "string"
          return "sp_StringIO_string(#{recv_code})"
        when "pos", "tell"
          return "sp_StringIO_pos(#{recv_code})"
        when "size", "length"
          return "sp_StringIO_size(#{recv_code})"
        when "rewind"
          emit("sp_StringIO_rewind(#{recv_code});")
          return "0"
        when "read"
          if args.length > 0
            val = compile_expr(args[0])
            return "sp_StringIO_read_n(#{recv_code}, #{val})"
          end
          return "sp_StringIO_read(#{recv_code})"
        when "gets"
          return "sp_StringIO_gets(#{recv_code})"
        when "getc"
          return "sp_StringIO_getc(#{recv_code})"
        when "getbyte"
          return "sp_StringIO_getbyte(#{recv_code})"
        when "seek"
          off = compile_expr(args[0])
          whence = args.length > 1 ? compile_expr(args[1]) : "0"
          emit("sp_StringIO_seek(#{recv_code}, #{off}, #{whence});")
          return "0"
        when "truncate"
          val = compile_expr(args[0])
          emit("sp_StringIO_truncate(#{recv_code}, #{val});")
          return "0"
        when "close"
          emit("sp_StringIO_close(#{recv_code});")
          return "0"
        when "eof?"
          return "sp_StringIO_eof_p(#{recv_code})"
        when "closed?"
          return "sp_StringIO_closed_p(#{recv_code})"
        when "flush"
          return "sp_StringIO_flush(#{recv_code})"
        when "sync"
          return "sp_StringIO_sync(#{recv_code})"
        when "isatty"
          return "sp_StringIO_isatty(#{recv_code})"
        when "lineno"
          return "sp_StringIO_lineno(#{recv_code})"
        end
      end

      case mname
      # ---- Regexp operators ----
      when "=~"
        if args[0].is_a?(Prism::RegularExpressionNode)
          re_var = register_regexp(args[0].unescaped)
          return "(sp_re_match(#{re_var}, #{mstr_recv}) >= 0)"
        end
        return "0"
      when "match?"
        if args[0].is_a?(Prism::RegularExpressionNode)
          re_var = register_regexp(args[0].unescaped)
          return "sp_re_match_p(#{re_var}, #{mstr_recv})"
        end
        return "0"
      when "scan"
        if args[0].is_a?(Prism::RegularExpressionNode) && node.block
          re_var = register_regexp(args[0].unescaped)
          scan_id = next_temp
          block_node = node.block
          block_params = block_node.parameters&.parameters&.requireds || []
          bparam = block_params[0]
          bparam_name = bparam.is_a?(Prism::RequiredParameterNode) ? bparam.name.to_s : "m"
          ensure_var_declared(bparam_name, Type::STRING)
          emit("{ /* scan */")
          @indent += 1
          emit("const char *_ss_#{scan_id} = #{mstr_recv};")
          emit("OnigRegion *_sr_#{scan_id} = onig_region_new();")
          emit("const OnigUChar *_se_#{scan_id} = (const OnigUChar *)_ss_#{scan_id} + strlen(_ss_#{scan_id});")
          emit("int _sp_#{scan_id} = 0;")
          emit("while (_sp_#{scan_id} >= 0) {")
          @indent += 1
          emit("_sp_#{scan_id} = onig_search(#{re_var}, (const OnigUChar *)_ss_#{scan_id}, _se_#{scan_id},")
          emit("  (const OnigUChar *)_ss_#{scan_id} + _sp_#{scan_id}, _se_#{scan_id}, _sr_#{scan_id}, ONIG_OPTION_NONE);")
          emit("if (_sp_#{scan_id} >= 0) {")
          @indent += 1
          emit("int _ml_#{scan_id} = _sr_#{scan_id}->end[0] - _sr_#{scan_id}->beg[0];")
          emit("char *lv_#{bparam_name} = (char *)malloc(_ml_#{scan_id} + 1);")
          emit("memcpy(lv_#{bparam_name}, _ss_#{scan_id} + _sr_#{scan_id}->beg[0], _ml_#{scan_id});")
          emit("lv_#{bparam_name}[_ml_#{scan_id}] = '\\0';")
          if block_node.body
            generate_body_stmts(block_node.body)
          end
          emit("_sp_#{scan_id} = _sr_#{scan_id}->end[0];")
          @indent -= 1
          emit("}")
          @indent -= 1
          emit("}")
          emit("onig_region_free(_sr_#{scan_id}, 1);")
          @indent -= 1
          emit("}")
          return "0"
        end
        return "0"
      # ---- Arithmetic operators ----
      when "+", "-", "*", "/", "%"
        arg = compile_expr(args[0])
        arg_type = infer_type(args[0])
        # Check if either operand is poly
        if recv_type == Type::POLY || arg_type == Type::POLY
          @needs_poly = true
          a = recv_type == Type::POLY ? recv_code : box_value(recv_code, recv_type)
          b = arg_type == Type::POLY ? arg : box_value(arg, arg_type)
          op_name = case mname
                    when "+" then "add"
                    when "-" then "sub"
                    when "*" then "mul"
                    when "/" then "div"
                    when "%" then "mod"
                    end
          return "sp_poly_#{op_name}(#{a}, #{b})"
        end
        if (recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING) && mname == "+"
          @string_helpers_needed << :str_concat
          return "sp_str_concat(#{mstr_recv}, #{arg})"
        elsif (recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING) && mname == "*"
          @string_helpers_needed << :str_repeat
          return "sp_str_repeat(#{mstr_recv}, #{arg})"
        end
        # Ruby integer division is floor division, C is truncation toward zero
        if mname == "/" && recv_type == Type::INTEGER && arg_type == Type::INTEGER
          return "sp_idiv(#{recv_code}, #{arg})"
        end
        # Ruby integer modulo follows floor division semantics
        if mname == "%" && recv_type == Type::INTEGER && arg_type == Type::INTEGER
          return "sp_imod(#{recv_code}, #{arg})"
        end
        return "(#{recv_code} #{mname} #{arg})"
      when "**"
        arg = compile_expr(args[0])
        if recv_type == Type::FLOAT || infer_type(args[0]) == Type::FLOAT
          return "pow(#{recv_code}, #{arg})"
        else
          return "((mrb_int)pow((double)#{recv_code}, (double)#{arg}))"
        end
      when "|", "&", "^"
        arg = compile_expr(args[0])
        return "(#{recv_code} #{mname} #{arg})"
      when ">>"
        arg = compile_expr(args[0])
        return "(#{recv_code} >> #{arg})"
      when "-@"
        return "(-#{recv_code})"
      when "+@"
        return "(+#{recv_code})"

      # ---- Comparison operators ----
      when "==", "!="
        arg_type = infer_type(args[0])
        arg = compile_expr(args[0])
        # nil comparison for strings/pointers
        if (recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING) && arg_type == Type::NIL
          op = mname == "==" ? "==" : "!="
          return "(#{recv_code} #{op} NULL)"
        elsif recv_type == Type::NIL && (arg_type == Type::STRING || arg_type == Type::MUTABLE_STRING)
          op = mname == "==" ? "==" : "!="
          return "(NULL #{op} #{arg})"
        elsif recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING || arg_type == Type::STRING
          op = mname == "==" ? "== 0" : "!= 0"
          return "(strcmp(#{mstr_recv}, #{arg}) #{op})"
        elsif recv_type == Type::ARRAY && mname == "!="
          return "sp_IntArray_neq(#{recv_code}, #{arg})"
        end
        return "(#{recv_code} #{mname} #{arg})"
      when "<", ">", "<=", ">="
        arg = compile_expr(args[0])
        arg_type = infer_type(args[0])
        if recv_type == Type::POLY || arg_type == Type::POLY
          @needs_poly = true
          a = recv_type == Type::POLY ? recv_code : box_value(recv_code, recv_type)
          b = arg_type == Type::POLY ? arg : box_value(arg, arg_type)
          op_name = case mname
                    when "<" then "lt"
                    when ">" then "gt"
                    when "<=" then "le"
                    when ">=" then "ge"
                    end
          return "sp_poly_#{op_name}(#{a}, #{b})"
        end
        if recv_type == Type::STRING
          op = { "<" => "< 0", ">" => "> 0", "<=" => "<= 0", ">=" => ">= 0" }[mname]
          return "(strcmp(#{recv_code}, #{arg}) #{op})"
        end
        return "(#{recv_code} #{mname} #{arg})"
      when "<=>"
        arg = compile_expr(args[0])
        if recv_type == Type::STRING
          return "strcmp(#{recv_code}, #{arg})"
        end
        tmp = "_cmp_#{next_temp}"
        return "((#{recv_code} > #{arg}) - (#{recv_code} < #{arg}))"

      # ---- Boolean operators ----
      when "&&", "and"
        arg = compile_expr(args[0])
        return "(#{recv_code} && #{arg})"
      when "||", "or"
        arg = compile_expr(args[0])
        return "(#{recv_code} || #{arg})"
      when "!"
        return "(!#{recv_code})"

      # ---- Integer methods ----
      when "abs"
        if recv_type == Type::FLOAT
          return "fabs(#{recv_code})"
        else
          return "(#{recv_code} < 0 ? -(#{recv_code}) : (#{recv_code}))"
        end
      when "to_i"
        if recv_type == Type::TIME
          @needs_time = true
          return "sp_Time_to_i(#{recv_code})"
        end
        if recv_type == Type::STRING
          return "((mrb_int)atoll(#{recv_code}))"
        end
        if recv_type == Type::MUTABLE_STRING
          return "((mrb_int)atoll(#{mstr_recv}))"
        end
        return "((mrb_int)(#{recv_code}))"
      when "clamp"
        if args.length >= 2
          lo = compile_expr(args[0])
          hi = compile_expr(args[1])
          return "((#{recv_code}) < (#{lo}) ? (#{lo}) : ((#{recv_code}) > (#{hi}) ? (#{hi}) : (#{recv_code})))"
        end
        return recv_code
      when "to_f"
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          return "atof(#{mstr_recv})"
        end
        return "((mrb_float)(#{recv_code}))"
      when "to_s"
        if recv_type == Type::POLY
          @needs_poly = true
          return "sp_poly_to_s(#{recv_code})"
        elsif recv_type == Type::INTEGER
          @string_helpers_needed << :int_to_s
          return "sp_int_to_s(#{recv_code})"
        elsif recv_type == Type::FLOAT
          @string_helpers_needed << :float_to_s
          return "sp_float_to_s(#{recv_code})"
        elsif recv_type == Type::BOOLEAN
          return "(#{recv_code} ? \"true\" : \"false\")"
        elsif recv_type == Type::STRING
          return recv_code
        elsif recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          return "sp_String_cstr(#{recv_code})"
        else
          # Object to_s
          if recv.is_a?(Prism::LocalVariableReadNode)
            v = lookup_var(recv.name.to_s)
            if v && @classes.key?(recv_type.to_s.delete_prefix("sp_"))
              # try class to_s method
            end
          end
          # Check if it's a class instance with to_s
          return try_class_method_call(recv, recv_code, "to_s", args) || "sp_int_to_s(#{recv_code})"
        end
      when "to_a"
        if recv_type == Type::RANGE
          @needs_int_array = true
          @needs_gc = true
          return "sp_Range_to_a(#{recv_code})"
        end
        return recv_code
      when "to_sym"
        # Symbols are just strings in AOT
        return recv_code
      when "ceil"
        return "((mrb_int)ceil(#{recv_code}))"
      when "floor"
        return "((mrb_int)floor(#{recv_code}))"
      when "round"
        return "((mrb_int)round(#{recv_code}))"
      when "even?"
        return "((#{recv_code}) % 2 == 0)"
      when "odd?"
        return "((#{recv_code}) % 2 != 0)"
      when "zero?"
        return "((#{recv_code}) == 0)"
      when "positive?"
        return "((#{recv_code}) > 0)"
      when "negative?"
        return "((#{recv_code}) < 0)"
      when "nil?"
        if recv.is_a?(Prism::NilNode)
          return "TRUE"
        end
        if recv_type == Type::NIL
          return "TRUE"
        end
        if recv_type == Type::POLY
          @needs_poly = true
          return "sp_poly_nil_p(#{recv_code})"
        end
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          return "(#{recv_code} == NULL)"
        end
        # Class-typed pointers are nullable (NULL = nil)
        if recv_type.is_a?(String) && @classes[recv_type]
          return "(#{recv_code} == NULL)"
        end
        return "FALSE"
      when "frozen?"
        return "TRUE"  # AOT: everything is frozen
      when "freeze"
        return recv_code  # no-op
      when "itself"
        return recv_code
      when "succ"
        return "((#{recv_code}) + 1)"

      # ---- String methods ----
      when "length", "size"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "ARGV"
          return "sp_Argv_length(&sp_argv)"
        end
        if recv_type == Type::STRING
          return "((mrb_int)strlen(#{recv_code}))"
        elsif recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          return "sp_String_length(#{recv_code})"
        elsif recv_type == Type::ARRAY || recv_type == Type::POLY_ARRAY
          @needs_int_array = true
          return "sp_IntArray_length(#{recv_code})"
        elsif recv_type == Type::FLOAT_ARRAY
          @needs_float_array = true
          return "sp_FloatArray_length(#{recv_code})"
        elsif recv_type == Type::STR_ARRAY
          @needs_str_array = true
          return "sp_StrArray_length(#{recv_code})"
        elsif recv_type == Type::HASH
          @needs_str_int_hash = true
          return "sp_StrIntHash_length(#{recv_code})"
        elsif recv_type == Type::STR_HASH
          @needs_str_str_hash = true
          return "sp_RbHash_length(#{recv_code})"
        elsif recv_type == Type::POLY_HASH
          @needs_poly_hash = true
          return "sp_PolyHash_length(#{recv_code})"
        else
          return "((mrb_int)strlen(#{recv_code}))"
        end
      when "upcase"
        @string_helpers_needed << :str_upcase
        return "sp_str_upcase(#{mstr_recv})"
      when "downcase"
        @string_helpers_needed << :str_downcase
        return "sp_str_downcase(#{mstr_recv})"
      when "strip"
        @string_helpers_needed << :str_strip
        return "sp_str_strip(#{mstr_recv})"
      when "chomp"
        @string_helpers_needed << :str_chomp
        return "sp_str_chomp(#{mstr_recv})"
      when "chop"
        @string_helpers_needed << :str_chop
        return "sp_str_chop(#{mstr_recv})"
      when "bytesize"
        if recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          return "sp_String_length(#{recv_code})"
        end
        return "((mrb_int)strlen(#{recv_code}))"
      when "getbyte"
        arg = compile_expr(args[0])
        if recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          return "((mrb_int)(unsigned char)sp_String_cstr(#{recv_code})[#{arg}])"
        end
        return "((mrb_int)(unsigned char)(#{recv_code})[#{arg}])"
      when "setbyte"
        idx = compile_expr(args[0])
        val = compile_expr(args[1])
        if recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          emit("sp_String_setbyte(#{recv_code}, #{idx}, #{val});")
          return val
        end
        return val  # no-op on immutable strings
      when "dup"
        if recv_type == Type::STRING
          @needs_mutable_string = true
          return "sp_String_new(#{recv_code})"
        elsif recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          return "sp_String_dup(#{recv_code})"
        elsif recv_type == Type::ARRAY
          @needs_int_array = true
          return "sp_IntArray_dup(#{recv_code})"
        elsif recv_type == Type::FLOAT_ARRAY
          @needs_float_array = true
          return "sp_FloatArray_dup(#{recv_code})"
        end
        return recv_code
      when "capitalize"
        @string_helpers_needed << :str_capitalize
        return "sp_str_capitalize(#{mstr_recv})"
      when "lstrip"
        @string_helpers_needed << :str_lstrip
        return "sp_str_lstrip(#{mstr_recv})"
      when "rstrip"
        @string_helpers_needed << :str_rstrip
        return "sp_str_rstrip(#{mstr_recv})"
      when "ljust"
        @string_helpers_needed << :str_ljust
        width = compile_expr(args[0])
        pad = args.length > 1 ? compile_expr(args[1]) : '" "'
        return "sp_str_ljust(#{mstr_recv}, #{width}, #{pad})"
      when "rjust"
        @string_helpers_needed << :str_rjust
        width = compile_expr(args[0])
        pad = args.length > 1 ? compile_expr(args[1]) : '" "'
        return "sp_str_rjust(#{mstr_recv}, #{width}, #{pad})"
      when "center"
        @string_helpers_needed << :str_center
        width = compile_expr(args[0])
        return "sp_str_center(#{mstr_recv}, #{width})"
      when "reverse"
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          @string_helpers_needed << :str_reverse
          return "sp_str_reverse(#{mstr_recv})"
        elsif recv_type == Type::ARRAY
          @needs_int_array = true
          tmp = "_rev_#{next_temp}"
          emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
          emit("for (mrb_int _i = sp_IntArray_length(#{recv_code}) - 1; _i >= 0; _i--) sp_IntArray_push(#{tmp}, sp_IntArray_get(#{recv_code}, _i));")
          return tmp
        end
        @string_helpers_needed << :str_reverse
        return "sp_str_reverse(#{mstr_recv})"
      when "reverse!"
        if recv_type == Type::ARRAY
          @needs_int_array = true
          emit("sp_IntArray_reverse_bang(#{recv_code});")
          return recv_code
        end
        return recv_code
      when "include?"
        arg = compile_expr(args[0])
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          return "(strstr(#{mstr_recv}, #{arg}) != NULL)"
        elsif recv_type == Type::RANGE
          @needs_range = true
          return "sp_Range_include_p(#{recv_code}, #{arg})"
        elsif recv_type == Type::ARRAY
          @needs_int_array = true
          @needs_gc = true
          # Generate inline include check
          tmp = "_incl_#{next_temp}"
          emit("mrb_bool #{tmp} = FALSE; { mrb_int _ii_#{tmp};")
          emit(" for (_ii_#{tmp} = 0; _ii_#{tmp} < sp_IntArray_length(#{recv_code}); _ii_#{tmp}++)")
          emit("  if (sp_IntArray_get(#{recv_code}, _ii_#{tmp}) == #{arg}) { #{tmp} = TRUE; break; }")
          emit("}")
          return tmp
        end
        return "(strstr(#{recv_code}, #{arg}) != NULL)"
      when "start_with?"
        @string_helpers_needed << :str_starts_with
        arg = compile_expr(args[0])
        return "sp_str_starts_with(#{mstr_recv}, #{arg})"
      when "end_with?"
        @string_helpers_needed << :str_ends_with
        arg = compile_expr(args[0])
        return "sp_str_ends_with(#{mstr_recv}, #{arg})"
      when "squeeze"
        @string_helpers_needed << :str_squeeze
        return "sp_str_squeeze(#{mstr_recv})"
      when "tr"
        @string_helpers_needed << :str_tr
        from = compile_expr(args[0])
        to = compile_expr(args[1])
        return "sp_str_tr(#{mstr_recv}, #{from}, #{to})"
      when "hex"
        return "((mrb_int)strtol(#{mstr_recv}, NULL, 16))"
      when "oct"
        return "((mrb_int)strtol(#{mstr_recv}, NULL, 8))"
      when "to_f"
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          return "atof(#{mstr_recv})"
        end
        return "((mrb_float)(#{recv_code}))"
      when "gsub"
        if args[0].is_a?(Prism::RegularExpressionNode)
          re_var = register_regexp(args[0].unescaped)
          to = compile_expr(args[1])
          return "sp_re_gsub(#{re_var}, #{mstr_recv}, #{to})"
        end
        @string_helpers_needed << :str_gsub
        from = compile_expr(args[0])
        to = compile_expr(args[1])
        return "sp_str_gsub(#{mstr_recv}, #{from}, #{to})"
      when "sub"
        if args[0].is_a?(Prism::RegularExpressionNode)
          re_var = register_regexp(args[0].unescaped)
          to = compile_expr(args[1])
          return "sp_re_sub(#{re_var}, #{mstr_recv}, #{to})"
        end
        @string_helpers_needed << :str_sub
        from = compile_expr(args[0])
        to = compile_expr(args[1])
        return "sp_str_sub(#{mstr_recv}, #{from}, #{to})"
      when "count"
        if recv_type == Type::STRING
          @string_helpers_needed << :str_count
          arg = compile_expr(args[0])
          return "sp_str_count(#{recv_code}, #{arg})"
        elsif recv_type == Type::RANGE && node.block
          # Range.count { |i| expr } -> for loop over range counting truthy block results
          params = block_params(node.block)
          bparam = params[0] || "_i"
          tmp = "_cnt_#{next_temp}"
          rng_tmp = "_rng_#{next_temp}"
          emit("mrb_int #{tmp} = 0;")
          emit("{ sp_Range #{rng_tmp} = #{recv_code};")
          emit("for (mrb_int lv_#{bparam} = #{rng_tmp}.first; lv_#{bparam} <= #{rng_tmp}.last; lv_#{bparam}++) {")
          @indent += 1
          push_scope
          declare_var(bparam, Type::INTEGER, c_name: "lv_#{bparam}")
          block_body = node.block.body
          val = compile_expr(block_body.is_a?(Prism::StatementsNode) ? block_body.body.last : block_body)
          emit("if (#{val}) #{tmp}++;")
          pop_scope
          @indent -= 1
          emit("}}")
          return tmp
        elsif recv_type == Type::ARRAY || recv_type == Type::STR_ARRAY
          if node.block
            return compile_count_with_block(node, recv_code, recv_type)
          else
            if recv_type == Type::ARRAY
              return "sp_IntArray_length(#{recv_code})"
            else
              return "sp_StrArray_length(#{recv_code})"
            end
          end
        end
        return "0"
      when "split"
        @needs_str_array = true
        if !args.empty? && args[0].is_a?(Prism::RegularExpressionNode)
          re_var = register_regexp(args[0].unescaped)
          return "sp_re_split(#{re_var}, #{mstr_recv})"
        end
        @string_helpers_needed << :str_split
        if args.empty?
          return "sp_str_split(#{mstr_recv}, \" \")"
        else
          arg = compile_expr(args[0])
          return "sp_str_split(#{mstr_recv}, #{arg})"
        end
      when "chars"
        @needs_str_array = true
        @string_helpers_needed << :str_chars
        return "sp_str_chars(#{recv_code})"
      when "bytes"
        @needs_int_array = true
        @needs_gc = true
        return "sp_str_bytes(#{recv_code})"
      when "join"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          compiled_args = args.map { |a| compile_expr(a) }
          return "sp_File_join(#{compiled_args.join(', ')})"
        end
        if recv_type == Type::ARRAY
          @needs_int_array = true
          sep = args.empty? ? '""' : compile_expr(args[0])
          return "sp_IntArray_join(#{recv_code}, #{sep})"
        elsif recv_type == Type::STR_ARRAY
          @needs_str_array = true
          sep = args.empty? ? '""' : compile_expr(args[0])
          # Generate inline join for StrArray
          tmp = "_sj_#{next_temp}"
          emit("const char *#{tmp}; { size_t _total_#{tmp} = 0; size_t _sl_#{tmp} = strlen(#{sep});")
          emit(" for (mrb_int _ji_#{tmp} = 0; _ji_#{tmp} < sp_StrArray_length(#{recv_code}); _ji_#{tmp}++) _total_#{tmp} += strlen((#{recv_code})->data[_ji_#{tmp}]) + _sl_#{tmp};")
          emit(" char *_jbuf_#{tmp} = (char *)malloc(_total_#{tmp} + 1); size_t _jp_#{tmp} = 0;")
          emit(" for (mrb_int _ji_#{tmp} = 0; _ji_#{tmp} < sp_StrArray_length(#{recv_code}); _ji_#{tmp}++) {")
          emit("  if (_ji_#{tmp} > 0) { memcpy(_jbuf_#{tmp} + _jp_#{tmp}, #{sep}, _sl_#{tmp}); _jp_#{tmp} += _sl_#{tmp}; }")
          emit("  size_t _el_#{tmp} = strlen((#{recv_code})->data[_ji_#{tmp}]); memcpy(_jbuf_#{tmp} + _jp_#{tmp}, (#{recv_code})->data[_ji_#{tmp}], _el_#{tmp}); _jp_#{tmp} += _el_#{tmp};")
          emit(" } _jbuf_#{tmp}[_jp_#{tmp}] = '\\0'; #{tmp} = _jbuf_#{tmp}; }")
          return tmp
        end
        return '""'
      when "replace"
        if recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          arg = compile_expr(args[0])
          emit("sp_String_replace(#{recv_code}, #{arg});")
          return recv_code
        end
        return recv_code
      when "clear"
        if recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          emit("sp_String_clear(#{recv_code});")
          return recv_code
        end
        return recv_code
      when "ord"
        return "((mrb_int)(unsigned char)#{recv_code}[0])"
      when "chr"
        tmp = "_chr_#{next_temp}"
        emit("char #{tmp}[2] = {(char)#{recv_code}, '\\0'};")
        return "(const char *)#{tmp}"

      # ---- String << (append) ----
      when "<<"
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          arg = compile_expr(args[0])
          emit("sp_String_append(#{recv_code}, #{arg});")
          return recv_code
        elsif recv_type == Type::ARRAY
          @needs_int_array = true
          arg = compile_expr(args[0])
          emit("sp_IntArray_push(#{recv_code}, #{arg});")
          return recv_code
        end
        # Integer bitwise left shift
        if recv_type == Type::INTEGER || recv_type == Type::UNKNOWN
          arg = compile_expr(args[0])
          return "(#{recv_code} << #{arg})"
        end
        return recv_code

      # ---- Array methods ----
      when "push"
        if recv_type == Type::ARRAY
          @needs_int_array = true
          arg = compile_expr(args[0])
          return "(sp_IntArray_push(#{recv_code}, #{arg}), #{recv_code})"
        elsif recv_type == Type::STR_ARRAY
          @needs_str_array = true
          arg = compile_expr(args[0])
          return "(sp_StrArray_push(#{recv_code}, #{arg}), #{recv_code})"
        end
        return recv_code
      when "pop"
        if recv_type == Type::STR_ARRAY
          @needs_str_array = true
          return "sp_StrArray_pop(#{recv_code})"
        end
        @needs_int_array = true
        return "sp_IntArray_pop(#{recv_code})"
      when "shift"
        @needs_int_array = true
        return "sp_IntArray_shift(#{recv_code})"
      when "unshift"
        @needs_int_array = true
        arg = compile_expr(args[0])
        return "sp_IntArray_unshift(#{recv_code}, #{arg})"
      when "first"
        if recv_type == Type::ARRAY
          @needs_int_array = true
          return "sp_IntArray_get(#{recv_code}, 0)"
        elsif recv_type == Type::RANGE
          @needs_range = true
          return "(#{recv_code}).first"
        end
        return "0"
      when "last"
        if recv_type == Type::ARRAY
          @needs_int_array = true
          return "sp_IntArray_get(#{recv_code}, sp_IntArray_length(#{recv_code}) - 1)"
        elsif recv_type == Type::STR_ARRAY
          @needs_str_array = true
          return "sp_StrArray_last(#{recv_code})"
        elsif recv_type == Type::RANGE
          @needs_range = true
          return "(#{recv_code}).last"
        end
        return "0"
      when "min"
        @needs_int_array = true
        tmp = "_min_#{next_temp}"
        emit("mrb_int #{tmp} = sp_IntArray_get(#{recv_code}, 0);")
        emit("for (mrb_int _mi_#{tmp} = 1; _mi_#{tmp} < sp_IntArray_length(#{recv_code}); _mi_#{tmp}++) {")
        emit(" mrb_int _v_#{tmp} = sp_IntArray_get(#{recv_code}, _mi_#{tmp});")
        emit(" if (_v_#{tmp} < #{tmp}) #{tmp} = _v_#{tmp};")
        emit("}")
        return tmp
      when "max"
        @needs_int_array = true
        tmp = "_max_#{next_temp}"
        emit("mrb_int #{tmp} = sp_IntArray_get(#{recv_code}, 0);")
        emit("for (mrb_int _mi_#{tmp} = 1; _mi_#{tmp} < sp_IntArray_length(#{recv_code}); _mi_#{tmp}++) {")
        emit(" mrb_int _v_#{tmp} = sp_IntArray_get(#{recv_code}, _mi_#{tmp});")
        emit(" if (_v_#{tmp} > #{tmp}) #{tmp} = _v_#{tmp};")
        emit("}")
        return tmp
      when "sum"
        tmp = "_sum_#{next_temp}"
        emit("mrb_int #{tmp} = 0;")
        if recv_type == Type::RANGE && node.block
          # Range.sum { |i| expr } -> for loop over range with block
          params = block_params(node.block)
          bparam = params[0] || "_i"
          rng_tmp = "_rng_#{next_temp}"
          emit("{ sp_Range #{rng_tmp} = #{recv_code};")
          emit("for (mrb_int lv_#{bparam} = #{rng_tmp}.first; lv_#{bparam} <= #{rng_tmp}.last; lv_#{bparam}++) {")
          @indent += 1
          push_scope
          declare_var(bparam, Type::INTEGER, c_name: "lv_#{bparam}")
          block_body = node.block.body
          val = compile_expr(block_body.is_a?(Prism::StatementsNode) ? block_body.body.last : block_body)
          emit("#{tmp} += #{val};")
          pop_scope
          @indent -= 1
          emit("}}")
        elsif recv_type == Type::RANGE
          # Range.sum without block
          rng_tmp = "_rng_#{next_temp}"
          emit("{ sp_Range #{rng_tmp} = #{recv_code};")
          emit("for (mrb_int _si_#{tmp} = #{rng_tmp}.first; _si_#{tmp} <= #{rng_tmp}.last; _si_#{tmp}++)")
          emit(" #{tmp} += _si_#{tmp}; }")
        else
          @needs_int_array = true
          emit("for (mrb_int _si_#{tmp} = 0; _si_#{tmp} < sp_IntArray_length(#{recv_code}); _si_#{tmp}++)")
          emit(" #{tmp} += sp_IntArray_get(#{recv_code}, _si_#{tmp});")
        end
        return tmp
      when "reduce", "inject"
        return compile_reduce(node, recv_code, recv_type)
      when "sort"
        if node.block
          return compile_sort_by(node, recv_code, recv_type)
        end
        @needs_int_array = true
        return "sp_IntArray_sort(#{recv_code})"
      when "sort!"
        @needs_int_array = true
        emit("sp_IntArray_sort_bang(#{recv_code});")
        return recv_code
      when "sort_by"
        return compile_sort_by(node, recv_code, recv_type)
      when "uniq"
        @needs_int_array = true
        return "sp_IntArray_uniq(#{recv_code})"
      when "dup"
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          return mstr_recv
        end
        @needs_int_array = true
        return "sp_IntArray_dup(#{recv_code})"
      when "empty?"
        if recv_type == Type::ARRAY
          @needs_int_array = true
          return "sp_IntArray_empty(#{recv_code})"
        elsif recv_type == Type::STRING
          return "(strlen(#{recv_code}) == 0)"
        end
        return "FALSE"
      when "compact", "flatten"
        return recv_code  # In typed AOT, no nils in int arrays

      when "zip"
        # a.zip(b) returns array of pairs; minimal impl returns IntArray with a.length elements
        if recv_type == Type::ARRAY && args.length > 0
          @needs_int_array = true
          @needs_gc = true
          arg = compile_expr(args[0])
          tmp = "_zip_#{next_temp}"
          emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
          idx = "_zi_#{tmp}"
          emit("for (mrb_int #{idx} = 0; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
          @indent += 1
          emit("sp_IntArray_push(#{tmp}, 0); /* zip pair placeholder */")
          @indent -= 1
          emit("}")
          return tmp
        end
        return "0 /* zip */"

      # ---- Array [] and []= ----
      when "[]"
        # ENV["key"] -> getenv("key")
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "ENV"
          arg = compile_expr(args[0])
          return "getenv(#{arg})"
        end
        # ARGV[i] -> sp_argv.data[i]
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "ARGV"
          arg = compile_expr(args[0])
          return "sp_argv.data[#{arg}]"
        end
        # Check for class-typed array element access
        elem_class = array_elem_class_for_receiver(recv)
        if elem_class
          arg = compile_expr(args[0])
          return "#{recv_code}[#{arg}]"
        end
        arg = compile_expr(args[0])
        if recv_type == Type::ARRAY
          @needs_int_array = true
          # Check for class-typed dynamic array
          dyn_elem_class = nil
          if recv.is_a?(Prism::InstanceVariableReadNode) && @current_class
            dyn_ivar = recv.name.to_s.delete_prefix("@")
            dyn_elem_class = @ivar_elem_types.dig(@current_class, dyn_ivar)
            # Only use cast for dynamic arrays (no fixed size)
            dyn_elem_class = nil if @ivar_array_sizes.dig(@current_class, dyn_ivar)
          elsif recv.is_a?(Prism::LocalVariableReadNode)
            dyn_elem_class = @array_elem_types[recv.name.to_s]
            dyn_elem_class = nil if @local_array_sizes[recv.name.to_s]
          end
          if dyn_elem_class && @classes[dyn_elem_class]
            dyn_ptr = class_needs_gc?(@classes[dyn_elem_class])
            dyn_c_type = dyn_ptr ? "sp_#{dyn_elem_class} *" : "sp_#{dyn_elem_class}"
            return "(#{dyn_c_type})sp_IntArray_get(#{recv_code}, #{arg})"
          end
          return "sp_IntArray_get(#{recv_code}, #{arg})"
        elsif recv_type == Type::FLOAT_ARRAY
          @needs_float_array = true
          return "sp_FloatArray_get(#{recv_code}, #{arg})"
        elsif recv_type == Type::STR_ARRAY
          @needs_str_array = true
          return "(#{recv_code})->data[#{arg}]"
        elsif recv_type == Type::MUTABLE_STRING
          @needs_mutable_string = true
          return "sp_String_char_at(#{recv_code}, #{arg})"
        elsif recv_type == Type::STRING
          arg_type = infer_type(args[0])
          if arg_type == Type::RANGE
            @needs_range = true
            @string_helpers_needed << :str_slice_range
            return "sp_str_slice_range(#{recv_code}, #{arg})"
          elsif args.length >= 2
            # s[start, len]
            arg2 = compile_expr(args[1])
            @string_helpers_needed << :str_slice
            return "sp_str_slice(#{recv_code}, #{arg}, #{arg2})"
          else
            @string_helpers_needed << :str_char_at
            return "sp_str_char_at(#{recv_code}, #{arg})"
          end
        elsif recv_type == Type::HASH
          @needs_str_int_hash = true
          val_class = hash_value_class_for_receiver(recv)
          if val_class
            needs_ptr = class_needs_gc?(@classes[val_class])
            if needs_ptr
              return "(sp_#{val_class} *)sp_StrIntHash_get(#{recv_code}, #{arg})"
            else
              return "(sp_#{val_class})sp_StrIntHash_get(#{recv_code}, #{arg})"
            end
          end
          return "sp_StrIntHash_get(#{recv_code}, #{arg})"
        elsif recv_type == Type::STR_HASH
          @needs_str_str_hash = true
          return "sp_RbHash_get(#{recv_code}, #{arg})"
        elsif recv_type == Type::POLY_HASH
          @needs_poly_hash = true
          @needs_poly = true
          return "sp_PolyHash_get(#{recv_code}, #{arg})"
        end
        return "0"
      when "[]="
        key = compile_expr(args[0])
        val = compile_expr(args[1])
        # Check for class-typed array element assignment
        elem_class = array_elem_class_for_receiver(recv)
        if elem_class
          emit("(#{recv_code}[#{key}] = #{val});")
          return val
        end
        if recv_type == Type::ARRAY
          @needs_int_array = true
          emit("sp_IntArray_set(#{recv_code}, #{key}, #{val});")
          return val
        elsif recv_type == Type::FLOAT_ARRAY
          @needs_float_array = true
          emit("sp_FloatArray_set(#{recv_code}, #{key}, #{val});")
          return val
        elsif recv_type == Type::STR_ARRAY
          @needs_str_array = true
          emit("sp_StrArray_set(#{recv_code}, #{key}, #{val});")
          return val
        elsif recv_type == Type::HASH
          @needs_str_int_hash = true
          return "sp_StrIntHash_set(#{recv_code}, #{key}, #{val})"
        elsif recv_type == Type::STR_HASH
          @needs_str_str_hash = true
          return "sp_RbHash_set(#{recv_code}, #{key}, #{val})"
        end
        return val

      # ---- Array iterators (each, map, select, reject) ----
      when "each"
        return compile_each(node, recv_code, recv_type)
      when "map", "collect"
        return compile_map(node, recv_code, recv_type)
      when "select", "filter"
        return compile_select(node, recv_code, recv_type)
      when "reject"
        return compile_reject(node, recv_code, recv_type)
      when "min_by"
        return compile_min_by(node, recv_code, recv_type)
      when "max_by"
        return compile_max_by(node, recv_code, recv_type)
      when "any?"
        return compile_any(node, recv_code, recv_type)
      when "all?"
        return compile_all(node, recv_code, recv_type)
      when "flat_map"
        return compile_map(node, recv_code, recv_type)  # simplified

      # ---- Integer iterators ----
      when "times"
        return compile_times(node, recv_code)
      when "upto"
        return compile_upto(node, recv_code)
      when "downto"
        return compile_downto(node, recv_code)

      # ---- Hash methods ----
      when "has_key?", "key?"
        @needs_str_int_hash = true
        arg = compile_expr(args[0])
        return "sp_StrIntHash_has_key(#{recv_code}, #{arg})"
      when "delete"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          arg = compile_expr(args[0])
          emit("remove(#{arg});")
          return "0"
        end
        if recv_type == Type::STRING || recv_type == Type::MUTABLE_STRING
          @string_helpers_needed << :str_delete
          arg = compile_expr(args[0])
          return "sp_str_delete_chars(#{mstr_recv}, #{arg})"
        end
        if recv_type == Type::HASH
          @needs_str_int_hash = true
          arg = compile_expr(args[0])
          return "sp_StrIntHash_delete(#{recv_code}, #{arg})"
        end
        return "0"
      when "keys"
        if recv_type == Type::HASH
          @needs_str_int_hash = true
          @needs_str_array = true
          # Return keys as StrArray
          tmp = "_keys_#{next_temp}"
          emit("sp_StrArray *#{tmp} = sp_StrArray_new();")
          emit("for (sp_HashEntry *_ke = #{recv_code}->first; _ke; _ke = _ke->order_next)")
          emit("  sp_StrArray_push(#{tmp}, _ke->key);")
          return tmp
        end
        return "0"
      when "values"
        if recv_type == Type::HASH
          @needs_str_int_hash = true
          @needs_int_array = true
          @needs_gc = true
          return "sp_StrIntHash_values(#{recv_code})"
        end
        return "0"
      when "merge"
        @needs_str_int_hash = true
        arg = compile_expr(args[0])
        return "sp_StrIntHash_merge(#{recv_code}, #{arg})"
      when "transform_values"
        if recv_type == Type::HASH && node.block
          @needs_str_int_hash = true
          @needs_gc = true
          tmp = "_tv_#{next_temp}"
          params = block_params(node.block)
          val_var = params[0] || "v"
          emit("sp_StrIntHash *#{tmp} = sp_StrIntHash_new();")
          emit("for (sp_HashEntry *_tve_#{tmp} = #{recv_code}->first; _tve_#{tmp}; _tve_#{tmp} = _tve_#{tmp}->order_next) {")
          @indent += 1
          push_scope
          declare_var(val_var, Type::INTEGER, c_name: "lv_#{val_var}")
          emit("mrb_int lv_#{val_var} = _tve_#{tmp}->value;")
          block_val = compile_block_expr(node.block)
          emit("sp_StrIntHash_set(#{tmp}, _tve_#{tmp}->key, #{block_val});")
          pop_scope
          @indent -= 1
          emit("}")
          return tmp
        end
        return "0 /* transform_values */"

      # ---- Range methods ----
      when "first"
        if recv_type == Type::RANGE
          @needs_range = true
          return "(#{recv_code}).first"
        end
        return "0"
      when "last"
        if recv_type == Type::RANGE
          @needs_range = true
          return "(#{recv_code}).last"
        end
        return "0"

      # ---- Math ----
      when "sqrt"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Math"
          arg = compile_expr(args[0])
          return "sqrt(#{arg})"
        end
        return "0"
      when "cos"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Math"
          arg = compile_expr(args[0])
          return "cos(#{arg})"
        end
        return "0"
      when "sin"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Math"
          arg = compile_expr(args[0])
          return "sin(#{arg})"
        end
        return "0"

      # ---- Dir methods ----
      when "home"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Dir"
          return "getenv(\"HOME\")"
        end
        return '""'

      # ---- Time methods ----
      when "now"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Time"
          @needs_time = true
          return "sp_Time_now()"
        end
        return "0"
      when "at"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "Time"
          @needs_time = true
          arg = compile_expr(args[0])
          return "sp_Time_at(#{arg})"
        end
        return "0"

      # ---- File class methods ----
      when "read"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          arg = compile_expr(args[0])
          return "sp_File_read(#{arg})"
        end
        return "0"
      when "write"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          path_arg = compile_expr(args[0])
          data_arg = compile_expr(args[1])
          return "sp_File_write(#{path_arg}, #{data_arg})"
        end
        return "0"
      when "exist?", "exists?"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          arg = compile_expr(args[0])
          return "sp_File_exist(#{arg})"
        end
        return "FALSE"
      when "open"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          return compile_file_open(node, args)
        end
        return "0"
      when "join"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          @needs_file = true
          compiled_args = args.map { |a| compile_expr(a) }
          return "sp_File_join(#{compiled_args.join(', ')})"
        end
        if recv_type == Type::ARRAY || recv_type == Type::STR_ARRAY
          arg = args.length > 0 ? compile_expr(args[0]) : '","'
          return "sp_StrArray_join(#{recv_code}, #{arg})"
        end
        return '""'
      when "basename"
        if recv.is_a?(Prism::ConstantReadNode) && recv.name.to_s == "File"
          arg = compile_expr(args[0])
          return "basename(#{arg})"
        end
        return '""'

      # ---- new ----
      when "new"
        return compile_new(node, recv, args)

      # ---- Object methods ----
      when "class"
        return '"Object"'
      when "is_a?"
        if args.length > 0 && args[0].is_a?(Prism::ConstantReadNode)
          check_class = args[0].name.to_s
          # Determine the receiver's class
          recv_class = nil
          if recv.is_a?(Prism::LocalVariableReadNode)
            vname = recv.name.to_s
            recv_class = @var_class_types && @var_class_types[vname]
          end
          if recv_class
            # Check class hierarchy
            cls = recv_class
            while cls
              return "TRUE" if cls == check_class
              ci = @classes[cls]
              cls = ci ? ci.parent : nil
            end
            return "FALSE"
          end
        end
        return "TRUE"
      when "respond_to?"
        if args.length > 0 && args[0].is_a?(Prism::SymbolNode)
          method_name = args[0].value
          # Determine the receiver's class
          recv_class = nil
          if recv.is_a?(Prism::LocalVariableReadNode)
            vname = recv.name.to_s
            recv_class = @var_class_types && @var_class_types[vname]
          end
          if recv_class
            # Check if method exists in class hierarchy
            cls = recv_class
            while cls
              ci = @classes[cls]
              if ci
                if ci.methods[method_name] || ci.attrs[:reader].include?(method_name) ||
                   ci.attrs[:accessor].include?(method_name)
                  return "TRUE"
                end
              end
              cls = ci ? ci.parent : nil
            end
            return "FALSE"
          end
        end
        return "TRUE"
      when "send"
        return "0"
      when "__method__"
        if @current_method
          return c_string_literal(@current_method.name)
        end
        return '""'
      when "inspect"
        if recv_type == Type::STRING
          return recv_code
        end
        return "sp_int_to_s(#{recv_code})"
      when "dup"
        return recv_code
      when "clone"
        return recv_code
      when "hash"
        return recv_code  # simplified

      # ---- method().call() ----
      when "call"
        # Check if receiver is a Proc
        if recv_type == Type::PROC
          @needs_proc = true
          compiled_args = args.map { |a| compile_expr(a) }
          return "sp_Proc_call(#{recv_code}, #{compiled_args[0] || '0'})"
        end
        # method(:name).call(args) - limited support
        target_method = nil
        if recv.is_a?(Prism::CallNode) && recv.name.to_s == "method"
          method_arg = call_args(recv)
          if method_arg.length > 0 && method_arg[0].is_a?(Prism::SymbolNode)
            target_method = method_arg[0].value
          end
        elsif recv.is_a?(Prism::LocalVariableReadNode)
          target_method = @method_refs[recv.name.to_s]
        end
        if target_method && @methods[target_method]
          compiled_args = args.map { |a| compile_expr(a) }
          mi = @methods[target_method]
          if mi.has_yield
            compiled_args << "NULL"
            compiled_args << "NULL"
          end
          return "sp_#{target_method}(#{compiled_args.join(', ')})"
        end
        return "0"

      else
        # Check class methods
        result = try_class_method_call(recv, recv_code, mname, args)
        return result if result

        # Check if receiver is a class instance and method is an attr accessor
        if recv.is_a?(Prism::LocalVariableReadNode)
          v = lookup_var(recv.name.to_s)
          if v
            # Try to find the class
            @classes.each do |cname, ci|
              all_ivars = collect_all_ivars(ci)
              if ci.attrs[:reader].include?(mname) || ci.attrs[:accessor].include?(mname)
                needs_ptr = class_needs_gc?(ci)
                accessor = needs_ptr ? "#{recv_code}->#{mname}" : "#{recv_code}.#{mname}"
                return accessor
              end
              if mname.end_with?("=") && (ci.attrs[:writer].include?(mname.chomp("=")) || ci.attrs[:accessor].include?(mname.chomp("=")))
                field = mname.chomp("=")
                needs_ptr = class_needs_gc?(ci)
                accessor = needs_ptr ? "#{recv_code}->#{field}" : "#{recv_code}.#{field}"
                val = compile_expr(args[0])
                return "(#{accessor} = #{val})"
              end
              if ci.methods[mname]
                needs_ptr = class_needs_gc?(ci)
                compiled_args = args.map { |a| compile_expr(a) }
                param_list = [recv_code] + compiled_args
                cmname = sanitize_method_name(mname)
                # Check if method is inherited
                actual_class = find_method_class(cname, mname)
                if actual_class && actual_class != cname
                  if needs_ptr
                    return "sp_#{actual_class}_#{cmname}((sp_#{actual_class} *)#{recv_code}#{compiled_args.empty? ? '' : ', ' + compiled_args.join(', ')})"
                  else
                    return "sp_#{actual_class}_#{cmname}(#{param_list.join(', ')})"
                  end
                end
                return "sp_#{cname}_#{cmname}(#{param_list.join(', ')})"
              end
            end
          end
        end

        return "0 /* #{mname} */"
      end
    end

    # Return the element class name for a class-typed array receiver, or nil
    def array_elem_class_for_receiver(recv)
      if recv.is_a?(Prism::LocalVariableReadNode)
        return @array_elem_types[recv.name.to_s]
      elsif recv.is_a?(Prism::InstanceVariableReadNode) && @current_class
        ivar = recv.name.to_s.delete_prefix("@")
        return @ivar_elem_types.dig(@current_class, ivar)
      end
      nil
    end

    def hash_value_class_for_receiver(recv)
      if recv.is_a?(Prism::LocalVariableReadNode)
        return @local_hash_value_types[recv.name.to_s]
      elsif recv.is_a?(Prism::InstanceVariableReadNode) && @current_class
        ivar = recv.name.to_s.delete_prefix("@")
        result = @ivar_hash_value_types.dig(@current_class, ivar)
        return result if result
        # Also check if this is a method call on a hash value (e.g., ci.methods)
        # where ci was obtained from a typed hash
      elsif recv.is_a?(Prism::CallNode)
        # e.g., @classes["Dog"].methods["foo"] - first part returns ClassInfo, then .methods is a hash of MethodInfo
        recv_type = infer_type(recv.receiver)
        if recv_type.is_a?(String) && @classes[recv_type]
          ci = @classes[recv_type]
          mname = recv.name.to_s
          # Check if the method/attr returns a hash that stores class instances
          if mname == "methods" || mname == "class_methods"
            return "MethodInfo" if @classes["MethodInfo"]
          end
        end
      end
      nil
    end

    def try_class_method_call(recv, recv_code, mname, args)
      # Check if receiver is a class constant (static method call)
      if recv.is_a?(Prism::ConstantReadNode)
        cname = recv.name.to_s
        if @classes[cname] && @classes[cname].class_methods[mname]
          compiled_args = args.map { |a| compile_expr(a) }
          param_str = compiled_args.empty? ? "" : compiled_args.join(", ")
          return "sp_#{cname}_#{sanitize_method_name(mname)}(#{param_str})"
        end
        # Check module class methods
        if @module_class_methods && @module_class_methods[cname] && @module_class_methods[cname][mname]
          compiled_args = args.map { |a| compile_expr(a) }
          param_str = compiled_args.empty? ? "" : compiled_args.join(", ")
          return "sp_#{cname}_#{mname}(#{param_str})"
        end
      end

      # Check if receiver is a known class instance
      if recv.is_a?(Prism::LocalVariableReadNode)
        vname = recv.name.to_s
        # Check var_class_types
        if @var_class_types && @var_class_types[vname]
          cname = @var_class_types[vname]
          if @classes[cname]
            return compile_instance_method(cname, recv_code, mname, args)
          end
        end
      end
      nil
    end

    def compile_instance_method(cname, recv_code, mname, args)
      ci = @classes[cname]
      needs_ptr = class_needs_gc?(ci)

      # Check attrs
      if ci.attrs[:reader].include?(mname) || ci.attrs[:accessor].include?(mname)
        return needs_ptr ? "#{recv_code}->#{mname}" : "#{recv_code}.#{mname}"
      end

      if mname.end_with?("=") && (ci.attrs[:writer].include?(mname.chomp("=")) || ci.attrs[:accessor].include?(mname.chomp("=")))
        field = mname.chomp("=")
        val = compile_expr(args[0])
        return needs_ptr ? "(#{recv_code}->#{field} = #{val})" : "(#{recv_code}.#{field} = #{val})"
      end

      # Check methods
      actual = find_method_class(cname, mname)
      if actual
        compiled_args = args.map { |a| compile_expr(a) }
        if actual != cname && needs_ptr
          param_list = ["(sp_#{actual} *)#{recv_code}"] + compiled_args
        else
          param_list = [recv_code] + compiled_args
        end
        return "sp_#{actual}_#{sanitize_method_name(mname)}(#{param_list.join(', ')})"
      end

      nil
    end

    def find_method_class(cname, mname)
      ci = @classes[cname]
      return cname if ci.methods[mname]
      if ci.parent && @classes[ci.parent]
        return find_method_class(ci.parent, mname)
      end
      nil
    end

    def compile_new(node, recv, args)
      return "0" unless recv.is_a?(Prism::ConstantReadNode)
      cname = recv.name.to_s

      # Proc.new { ... }
      if cname == "Proc" && node.block
        return compile_proc_creation(node.block)
      end

      case cname
      when "Array"
        @needs_gc = true
        if args.length >= 2
          # Detect float array from default value
          default_node = node.arguments.arguments[1]
          is_float = default_node.is_a?(Prism::FloatNode) || infer_type(default_node) == Type::FLOAT
          n = compile_expr(args[0])
          val = compile_expr(args[1])
          tmp = "_arr_#{next_temp}"
          if is_float
            @needs_float_array = true
            emit("sp_FloatArray *#{tmp} = sp_FloatArray_new();")
            emit("for (mrb_int _i = 0; _i < #{n}; _i++) sp_FloatArray_push(#{tmp}, #{val});")
          else
            @needs_int_array = true
            emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
            emit("for (mrb_int _i = 0; _i < #{n}; _i++) sp_IntArray_push(#{tmp}, #{val});")
          end
          return tmp
        elsif args.length == 1
          @needs_int_array = true
          n = compile_expr(args[0])
          tmp = "_arr_#{next_temp}"
          emit("sp_IntArray *#{tmp} = sp_IntArray_new();")
          emit("for (mrb_int _i = 0; _i < #{n}; _i++) sp_IntArray_push(#{tmp}, 0);")
          return tmp
        end
        return "sp_IntArray_new()"
      when "Hash"
        @needs_str_int_hash = true
        @needs_gc = true
        if args.length > 0
          val = compile_expr(args[0])
          return "sp_StrIntHash_new_with_default(#{val})"
        end
        return "sp_StrIntHash_new()"
      when "StringIO"
        @needs_stringio = true
        if args.length > 0
          val = compile_expr(args[0])
          return "sp_StringIO_new_s(#{val})"
        end
        return "sp_StringIO_new()"
      end

      if @classes[cname]
        # Handle keyword arguments for struct-like constructors
        if args.length == 1 && args[0].is_a?(Prism::KeywordHashNode) && @struct_classes && @struct_classes[cname]
          fields = @struct_classes[cname]
          # Extract keyword args in field order
          kw_hash = {}
          args[0].elements.each do |assoc|
            if assoc.is_a?(Prism::AssocNode)
              key = assoc.key.is_a?(Prism::SymbolNode) ? assoc.key.value : assoc.key.to_s
              kw_hash[key] = compile_expr(assoc.value)
            end
          end
          compiled_args = fields.map { |f| kw_hash[f] || default_val(Type::INTEGER) }
          return "sp_#{cname}_new(#{compiled_args.join(', ')})"
        end

        compiled_args = args.map { |a| compile_expr(a) }
        return "sp_#{cname}_new(#{compiled_args.join(', ')})"
      end

      "0"
    end

    # ---- Block/iterator compilation ----
    def compile_each(node, recv_code, recv_type)
      return recv_code unless node.block

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      if recv_type == Type::RANGE
        @needs_range = true
        ensure_var_declared(iter_var, Type::INTEGER)
        rng_tmp = "_rng_#{next_temp}"
        emit("{ sp_Range #{rng_tmp} = #{recv_code};")
        emit("for (mrb_int lv_#{iter_var} = #{rng_tmp}.first; lv_#{iter_var} <= #{rng_tmp}.last; lv_#{iter_var}++) {")
        @indent += 1
        generate_block_body(block)
        @indent -= 1
        emit("}")
        emit("}")
      elsif recv_type == Type::HASH
        @needs_str_int_hash = true
        params2 = params[1] || "v"
        ensure_var_declared(iter_var, Type::STRING)
        val_class = hash_value_class_for_receiver(node.receiver)
        if val_class
          needs_ptr = class_needs_gc?(@classes[val_class])
          val_c_type = needs_ptr ? "sp_#{val_class} *" : "sp_#{val_class}"
          ensure_var_declared(params2, val_class)
        else
          ensure_var_declared(params2, Type::INTEGER)
        end
        tmp = "_he_#{next_temp}"
        emit("for (sp_HashEntry *#{tmp} = #{recv_code}->first; #{tmp}; #{tmp} = #{tmp}->order_next) {")
        @indent += 1
        emit("lv_#{iter_var} = #{tmp}->key;")
        if val_class
          emit("lv_#{params2} = (#{val_c_type})#{tmp}->value;")
        else
          emit("lv_#{params2} = #{tmp}->value;")
        end
        generate_block_body(block)
        @indent -= 1
        emit("}")
      elsif recv_type == Type::POLY_ARRAY
        @needs_int_array = true
        @needs_poly = true
        idx = "_ei_#{next_temp}"
        emit("for (mrb_int #{idx} = 0; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
        @indent += 1
        emit("sp_RbValue lv_#{iter_var} = (sp_RbValue)sp_IntArray_get(#{recv_code}, #{idx});")
        declare_var(iter_var, Type::POLY, c_name: "lv_#{iter_var}")
        generate_block_body(block)
        @indent -= 1
        emit("}")
      elsif recv_type == Type::ARRAY
        @needs_int_array = true
        elem_class = array_elem_class_for_receiver(node.receiver)
        idx = "_ei_#{next_temp}"
        emit("for (mrb_int #{idx} = 0; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
        @indent += 1
        if elem_class
          needs_ptr = class_needs_gc?(@classes[elem_class])
          elem_c_type = needs_ptr ? "sp_#{elem_class} *" : "sp_#{elem_class}"
          emit("#{elem_c_type} lv_#{iter_var} = (#{elem_c_type})sp_IntArray_get(#{recv_code}, #{idx});")
          declare_var(iter_var, elem_class, c_name: "lv_#{iter_var}")
        else
          emit("mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, #{idx});")
          declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
        end
        generate_block_body(block)
        @indent -= 1
        emit("}")
      elsif recv_type == Type::STR_ARRAY
        @needs_str_array = true
        idx = "_ei_#{next_temp}"
        emit("for (mrb_int #{idx} = 0; #{idx} < sp_StrArray_length(#{recv_code}); #{idx}++) {")
        @indent += 1
        emit("const char *lv_#{iter_var} = (#{recv_code})->data[#{idx}];")
        declare_var(iter_var, Type::STRING, c_name: "lv_#{iter_var}")
        generate_block_body(block)
        @indent -= 1
        emit("}")
      elsif recv_type == Type::STR_HASH
        @needs_str_str_hash = true
        params2 = params[1] || "v"
        ensure_var_declared(iter_var, Type::STRING)
        ensure_var_declared(params2, Type::STRING)
        tmp = "_he_#{next_temp}"
        emit("for (sp_RbHashEntry *#{tmp} = #{recv_code}->first; #{tmp}; #{tmp} = #{tmp}->order_next) {")
        @indent += 1
        emit("lv_#{iter_var} = #{tmp}->key;")
        emit("lv_#{params2} = #{tmp}->value;")
        generate_block_body(block)
        @indent -= 1
        emit("}")
      elsif recv_type == Type::POLY_HASH
        @needs_poly_hash = true
        @needs_poly = true
        params2 = params[1] || "v"
        ensure_var_declared(iter_var, Type::STRING)
        ensure_var_declared(params2, Type::POLY)
        tmp = "_he_#{next_temp}"
        emit("for (sp_PolyHashEntry *#{tmp} = #{recv_code}->first; #{tmp}; #{tmp} = #{tmp}->order_next) {")
        @indent += 1
        emit("lv_#{iter_var} = #{tmp}->key;")
        emit("lv_#{params2} = #{tmp}->value;")
        generate_block_body(block)
        @indent -= 1
        emit("}")
      end
      "0"
    end

    def compile_map(node, recv_code, recv_type)
      return recv_code unless node.block

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      @needs_int_array = true
      @needs_gc = true
      result = "_map_#{next_temp}"
      idx = "_mi_#{result}"

      # Detect n.times.map { block } chain
      is_times_map = node.receiver.is_a?(Prism::CallNode) && node.receiver.name.to_s == "times" && !node.receiver.block
      if is_times_map
        times_recv = compile_expr(node.receiver.receiver)
        emit("sp_IntArray *#{result} = sp_IntArray_new();")
        emit("for (mrb_int #{idx} = 0; #{idx} < #{times_recv}; #{idx}++) {")
        @indent += 1
        emit("mrb_int lv_#{iter_var} = #{idx};")
      elsif recv_type == Type::RANGE
        rng_tmp = "_rng_#{next_temp}"
        emit("sp_IntArray *#{result} = sp_IntArray_new();")
        emit("{ sp_Range #{rng_tmp} = #{recv_code};")
        emit("for (mrb_int #{idx} = #{rng_tmp}.first; #{idx} <= #{rng_tmp}.last; #{idx}++) {")
        @indent += 1
        emit("mrb_int lv_#{iter_var} = #{idx};")
      else
        emit("sp_IntArray *#{result} = sp_IntArray_new();")
        emit("for (mrb_int #{idx} = 0; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
        @indent += 1
        emit("mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, #{idx});")
      end
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")

      # Compile block body to get the expression
      body_val = compile_block_expr(block)
      emit("sp_IntArray_push(#{result}, #{body_val});")

      pop_scope
      @indent -= 1
      emit("}")
      if recv_type == Type::RANGE && !is_times_map
        emit("}")  # close the sp_Range block
      end
      result
    end

    def compile_select(node, recv_code, recv_type)
      return recv_code unless node.block

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      @needs_int_array = true
      @needs_gc = true
      result = "_sel_#{next_temp}"
      idx = "_si_#{result}"

      emit("sp_IntArray *#{result} = sp_IntArray_new();")
      emit("for (mrb_int #{idx} = 0; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
      @indent += 1
      emit("mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, #{idx});")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")

      body_val = compile_block_expr(block)
      emit("if (#{body_val}) sp_IntArray_push(#{result}, lv_#{iter_var});")

      pop_scope
      @indent -= 1
      emit("}")
      result
    end

    def compile_reject(node, recv_code, recv_type)
      return recv_code unless node.block

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      @needs_int_array = true
      @needs_gc = true
      result = "_rej_#{next_temp}"
      idx = "_ri_#{result}"

      emit("sp_IntArray *#{result} = sp_IntArray_new();")
      emit("for (mrb_int #{idx} = 0; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
      @indent += 1
      emit("mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, #{idx});")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")

      body_val = compile_block_expr(block)
      emit("if (!(#{body_val})) sp_IntArray_push(#{result}, lv_#{iter_var});")

      pop_scope
      @indent -= 1
      emit("}")
      result
    end

    def compile_proc_creation(block)
      @needs_proc = true
      @needs_block_fn = true
      blk_id = next_block_id
      blk_fn = "_blk_#{blk_id}"
      blk_env_type = "_blk_#{blk_id}_env"
      params_b = block_params(block)
      bparam = params_b[0] || "_x"

      # Emit block function
      @block_defs << "typedef struct { int _dummy; } #{blk_env_type};"
      @block_defs << "static mrb_int #{blk_fn}(void *_env, mrb_int _arg) {"
      @block_defs << "    #{blk_env_type} *_e = (#{blk_env_type} *)_env;"
      @block_defs << "    mrb_int lv_#{bparam} = _arg;"

      old_main = @in_main
      old_func = @func_bodies
      @in_main = false
      @func_bodies = []
      push_scope
      declare_var(bparam, Type::INTEGER, c_name: "lv_#{bparam}")
      blk_val = compile_block_expr(block)
      pop_scope
      @block_defs.concat(@func_bodies.map { |l| "  " + l.strip })
      @func_bodies = old_func
      @in_main = old_main
      @block_defs << "  return #{blk_val};"
      @block_defs << "    return 0;"
      @block_defs << "}"
      @block_defs << ""

      # Create sp_Proc on heap
      proc_tmp = "_proc_#{blk_id}"
      emit("sp_Proc *#{proc_tmp} = sp_Proc_new((sp_block_fn)#{blk_fn}, NULL);")
      return proc_tmp
    end

    def compile_catch(node, args)
      @needs_exception = true
      @needs_catch_throw = true

      tag = args.length > 0 && args[0].is_a?(Prism::SymbolNode) ? args[0].value : "tag"
      tag_c = c_string_literal(tag)

      result_var = "_catch_#{next_temp}"
      emit("mrb_int #{result_var}; {")
      @indent += 1
      emit("sp_exc_depth++;")
      jmp_var = "_cj_#{result_var}"
      emit("int #{jmp_var} = setjmp(sp_exc_stack[sp_exc_depth - 1]);")
      emit("if (#{jmp_var} == 0) {")
      @indent += 1

      # Generate block body
      if node.block
        block = node.block
        if block.body
          stmts = block.body.is_a?(Prism::StatementsNode) ? block.body.body : [block.body]
          if stmts.length > 0
            stmts[0..-2].each { |s| generate_stmt(s) }
            last_val = compile_expr(stmts.last)
            emit("#{result_var} = #{last_val};")
          else
            emit("#{result_var} = 0;")
          end
        else
          emit("#{result_var} = 0;")
        end
      else
        emit("#{result_var} = 0;")
      end

      emit("sp_exc_depth--;")
      @indent -= 1
      emit("}")
      emit("else if (#{jmp_var} == 2 && sp_throw_tag && strcmp(sp_throw_tag, #{tag_c}) == 0) {")
      @indent += 1
      emit("sp_exc_depth--;")
      emit("#{result_var} = sp_throw_is_str ? 0 : sp_throw_value_i;")
      @indent -= 1
      emit("}")
      emit("else {")
      @indent += 1
      emit("sp_exc_depth--;")
      emit("if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], #{jmp_var});")
      @indent -= 1
      emit("}")
      @indent -= 1
      emit("}")

      result_var
    end

    def compile_throw(node, args)
      @needs_exception = true
      @needs_catch_throw = true

      tag = args.length > 0 && args[0].is_a?(Prism::SymbolNode) ? args[0].value : "tag"
      tag_c = c_string_literal(tag)

      if args.length > 1
        val_type = infer_type(args[1])
        val = compile_expr(args[1])
        if val_type == Type::STRING
          emit("sp_throw_s(#{tag_c}, #{val});")
        else
          emit("sp_throw_i(#{tag_c}, #{val});")
        end
      else
        emit("sp_throw_i(#{tag_c}, 0);")
      end
      "0"
    end

    def compile_file_open(node, args)
      path_arg = compile_expr(args[0])
      mode_arg = args.length > 1 ? compile_expr(args[1]) : '"r"'

      if node.block
        block = node.block
        params = block_params(block)
        fvar = params[0] || "f"

        emit("{")
        @indent += 1
        emit("sp_File *lv_#{fvar} = sp_File_open(#{path_arg}, #{mode_arg});")
        push_scope
        declare_var(fvar, Type::FILE_OBJ, c_name: "lv_#{fvar}")

        # Generate block body, but handle f.puts / f.each_line specially
        if block.body
          stmts = block.body.is_a?(Prism::StatementsNode) ? block.body.body : [block.body]
          stmts.each { |s| generate_stmt(s) }
        end

        pop_scope
        emit("sp_File_close(lv_#{fvar});")
        @indent -= 1
        emit("}")
        return "0"
      else
        # No block - return file object
        return "sp_File_open(#{path_arg}, #{mode_arg})"
      end
    end

    def compile_reduce(node, recv_code, recv_type)
      return "0" unless node.block
      block = node.block
      params = block_params(block)
      acc_var = params[0] || "acc"
      elem_var = params[1] || "x"
      args = call_args(node)

      @needs_int_array = true

      acc_tmp = "_reduce_#{next_temp}"
      # Initial value
      if args.length > 0
        init_val = compile_expr(args[0])
        emit("mrb_int #{acc_tmp} = #{init_val};")
      else
        emit("mrb_int #{acc_tmp} = sp_IntArray_get(#{recv_code}, 0);")
      end

      idx = "_ri_#{acc_tmp}"
      start_idx = args.length > 0 ? "0" : "1"
      emit("for (mrb_int #{idx} = #{start_idx}; #{idx} < sp_IntArray_length(#{recv_code}); #{idx}++) {")
      @indent += 1
      push_scope
      ensure_var_declared(acc_var, Type::INTEGER)
      emit("lv_#{acc_var} = #{acc_tmp};")
      emit("mrb_int lv_#{elem_var} = sp_IntArray_get(#{recv_code}, #{idx});")
      declare_var(elem_var, Type::INTEGER, c_name: "lv_#{elem_var}")
      body_val = compile_block_expr(block)
      emit("#{acc_tmp} = #{body_val};")
      pop_scope
      @indent -= 1
      emit("}")
      acc_tmp
    end

    def compile_sort_by(node, recv_code, recv_type)
      # sort_by { |x| expr } -> compute key for each, then sort
      return recv_code unless node.block
      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      @needs_int_array = true
      @needs_gc = true

      # Simple approach: create pairs array, sort, extract values
      result = "_sortby_#{next_temp}"
      n_var = "_n_#{result}"
      keys_var = "_keys_#{result}"
      vals_var = "_vals_#{result}"

      emit("mrb_int #{n_var} = sp_IntArray_length(#{recv_code});")
      emit("mrb_int *#{keys_var} = (mrb_int *)malloc(sizeof(mrb_int) * #{n_var});")
      emit("mrb_int *#{vals_var} = (mrb_int *)malloc(sizeof(mrb_int) * #{n_var});")
      emit("for (mrb_int _i = 0; _i < #{n_var}; _i++) {")
      @indent += 1
      emit("mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, _i);")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val = compile_block_expr(block)
      emit("#{keys_var}[_i] = #{body_val};")
      emit("#{vals_var}[_i] = lv_#{iter_var};")
      pop_scope
      @indent -= 1
      emit("}")
      # Bubble sort by keys
      emit("for (mrb_int _i = 0; _i < #{n_var} - 1; _i++)")
      emit("  for (mrb_int _j = _i + 1; _j < #{n_var}; _j++)")
      emit("    if (#{keys_var}[_j] < #{keys_var}[_i]) {")
      emit("      mrb_int _t = #{keys_var}[_i]; #{keys_var}[_i] = #{keys_var}[_j]; #{keys_var}[_j] = _t;")
      emit("      _t = #{vals_var}[_i]; #{vals_var}[_i] = #{vals_var}[_j]; #{vals_var}[_j] = _t;")
      emit("    }")
      emit("sp_IntArray *#{result} = sp_IntArray_new();")
      emit("for (mrb_int _i = 0; _i < #{n_var}; _i++) sp_IntArray_push(#{result}, #{vals_var}[_i]);")
      emit("free(#{keys_var}); free(#{vals_var});")
      result
    end

    def compile_min_by(node, recv_code, recv_type)
      return "0" unless node.block
      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      @needs_int_array = true
      tmp = "_minby_#{next_temp}"
      emit("mrb_int #{tmp} = sp_IntArray_get(#{recv_code}, 0);")
      emit("mrb_int _minkey_#{tmp};")
      emit("{ mrb_int lv_#{iter_var} = #{tmp};")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val = compile_block_expr(block)
      emit("_minkey_#{tmp} = #{body_val}; }")
      pop_scope
      emit("for (mrb_int _i = 1; _i < sp_IntArray_length(#{recv_code}); _i++) {")
      emit("  mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, _i);")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val2 = compile_block_expr(block)
      emit("  mrb_int _k = #{body_val2};")
      emit("  if (_k < _minkey_#{tmp}) { _minkey_#{tmp} = _k; #{tmp} = lv_#{iter_var}; }")
      pop_scope
      emit("}")
      tmp
    end

    def compile_max_by(node, recv_code, recv_type)
      return "0" unless node.block
      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"

      @needs_int_array = true
      tmp = "_maxby_#{next_temp}"
      emit("mrb_int #{tmp} = sp_IntArray_get(#{recv_code}, 0);")
      emit("mrb_int _maxkey_#{tmp};")
      emit("{ mrb_int lv_#{iter_var} = #{tmp};")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val = compile_block_expr(block)
      emit("_maxkey_#{tmp} = #{body_val}; }")
      pop_scope
      emit("for (mrb_int _i = 1; _i < sp_IntArray_length(#{recv_code}); _i++) {")
      emit("  mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, _i);")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val2 = compile_block_expr(block)
      emit("  mrb_int _k = #{body_val2};")
      emit("  if (_k > _maxkey_#{tmp}) { _maxkey_#{tmp} = _k; #{tmp} = lv_#{iter_var}; }")
      pop_scope
      emit("}")
      tmp
    end

    def compile_any(node, recv_code, recv_type)
      return "FALSE" unless node.block
      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"
      tmp = "_any_#{next_temp}"
      emit("mrb_bool #{tmp} = FALSE;")
      emit("for (mrb_int _i = 0; _i < sp_IntArray_length(#{recv_code}); _i++) {")
      emit("  mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, _i);")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val = compile_block_expr(block)
      emit("  if (#{body_val}) { #{tmp} = TRUE; break; }")
      pop_scope
      emit("}")
      tmp
    end

    def compile_all(node, recv_code, recv_type)
      return "TRUE" unless node.block
      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"
      tmp = "_all_#{next_temp}"
      emit("mrb_bool #{tmp} = TRUE;")
      emit("for (mrb_int _i = 0; _i < sp_IntArray_length(#{recv_code}); _i++) {")
      emit("  mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, _i);")
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      body_val = compile_block_expr(block)
      emit("  if (!(#{body_val})) { #{tmp} = FALSE; break; }")
      pop_scope
      emit("}")
      tmp
    end

    def compile_count_with_block(node, recv_code, recv_type)
      block = node.block
      params = block_params(block)
      iter_var = params[0] || "x"
      tmp = "_cnt_#{next_temp}"
      emit("mrb_int #{tmp} = 0;")

      if recv_type == Type::ARRAY
        @needs_int_array = true
        emit("for (mrb_int _i = 0; _i < sp_IntArray_length(#{recv_code}); _i++) {")
        emit("  mrb_int lv_#{iter_var} = sp_IntArray_get(#{recv_code}, _i);")
      elsif recv_type == Type::STR_ARRAY
        @needs_str_array = true
        emit("for (mrb_int _i = 0; _i < sp_StrArray_length(#{recv_code}); _i++) {")
        emit("  const char *lv_#{iter_var} = (#{recv_code})->data[_i];")
      end

      push_scope
      elem_type = recv_type == Type::STR_ARRAY ? Type::STRING : Type::INTEGER
      declare_var(iter_var, elem_type, c_name: "lv_#{iter_var}")
      body_val = compile_block_expr(block)
      emit("  if (#{body_val}) #{tmp}++;")
      pop_scope
      emit("}")
      tmp
    end

    def compile_times(node, recv_code)
      return "0" unless node.block

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "_i"

      if iter_var != "_i"
        ensure_var_declared(iter_var, Type::INTEGER)
      end

      idx = "_ti_#{next_temp}"
      emit("for (mrb_int #{idx} = 0; #{idx} < #{recv_code}; #{idx}++) {")
      @indent += 1
      if iter_var != "_i"
        emit("lv_#{iter_var} = #{idx};")
      end
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      generate_block_body(block)
      pop_scope
      @indent -= 1
      emit("}")
      "0"
    end

    def compile_upto(node, recv_code)
      return "0" unless node.block
      args = call_args(node)
      upper = compile_expr(args[0])

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "i"
      ensure_var_declared(iter_var, Type::INTEGER)

      emit("for (lv_#{iter_var} = #{recv_code}; lv_#{iter_var} <= #{upper}; lv_#{iter_var}++) {")
      @indent += 1
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      generate_block_body(block)
      pop_scope
      @indent -= 1
      emit("}")
      "0"
    end

    def compile_downto(node, recv_code)
      return "0" unless node.block
      args = call_args(node)
      lower = compile_expr(args[0])

      block = node.block
      params = block_params(block)
      iter_var = params[0] || "i"
      ensure_var_declared(iter_var, Type::INTEGER)

      emit("for (lv_#{iter_var} = #{recv_code}; lv_#{iter_var} >= #{lower}; lv_#{iter_var}--) {")
      @indent += 1
      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      generate_block_body(block)
      pop_scope
      @indent -= 1
      emit("}")
      "0"
    end

    def compile_block_call(block_node, mi, method_name, compiled_args)
      # Create block struct and function for yield-based methods
      @needs_block_fn = true
      blk = block_node
      params = block_params(blk)
      iter_var = params[0] || "_arg"

      blk_id = next_block_id

      # Collect captured variables from outer scope
      captured = collect_captured_vars(blk.body, params)

      # Generate block struct
      env_name = "_blk_#{blk_id}_env"
      struct_lines = ["typedef struct { "]
      captured.each do |cv|
        struct_lines[0] += "#{c_type(cv[:type])} *#{cv[:name]}; "
      end
      struct_lines[0] += "} #{env_name};"

      # Generate block function
      func_lines = []
      func_lines << "static mrb_int _blk_#{blk_id}(void *_env, mrb_int _arg) {"
      func_lines << "    #{env_name} *_e = (#{env_name} *)_env;"
      func_lines << "    mrb_int lv_#{iter_var} = _arg;"

      # Compile block body to string
      old_main = @in_main
      old_indent = @indent
      old_bodies = @func_bodies
      @func_bodies = []
      @in_main = false
      @indent = 1

      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      generate_block_body_with_env(blk, captured)
      pop_scope

      block_code = @func_bodies
      @func_bodies = old_bodies
      @in_main = old_main
      @indent = old_indent

      block_code.each { |l| func_lines << l }
      func_lines << "    return 0;"
      func_lines << "}"

      @block_defs << struct_lines.join("\n")
      @block_defs << func_lines.join("\n")

      # Generate call with block env
      env_var = "_env_#{blk_id}"
      env_init = captured.map { |cv| "&#{cv[:c_name]}" }.join(", ")
      emit("#{env_name} #{env_var} = { #{env_init} };")
      return "sp_#{method_name}(#{(compiled_args + ["(sp_block_fn)_blk_#{blk_id}", "&#{env_var}"]).join(', ')})"
    end

    def compile_block_call_for_class(block_node, mi, class_name, cmname, param_list)
      # Similar to compile_block_call but for class method calls
      @needs_block_fn = true
      blk = block_node
      params = block_params(blk)
      iter_var = params[0] || "_arg"

      blk_id = next_block_id

      # Collect captured variables from outer scope
      captured = collect_captured_vars(blk.body, params)

      # Generate block struct
      env_name = "_blk_#{blk_id}_env"
      struct_lines = ["typedef struct { "]
      captured.each do |cv|
        struct_lines[0] += "#{c_type(cv[:type])} *#{cv[:name]}; "
      end
      struct_lines[0] += "} #{env_name};"

      # Generate block function
      func_lines = []
      func_lines << "static mrb_int _blk_#{blk_id}(void *_env, mrb_int _arg) {"
      func_lines << "    #{env_name} *_e = (#{env_name} *)_env;"
      func_lines << "    mrb_int lv_#{iter_var} = _arg;"

      old_main = @in_main
      old_indent = @indent
      old_bodies = @func_bodies
      @func_bodies = []
      @in_main = false
      @indent = 1

      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      generate_block_body_with_env(blk, captured)
      pop_scope

      block_code = @func_bodies
      @func_bodies = old_bodies
      @in_main = old_main
      @indent = old_indent

      block_code.each { |l| func_lines << l }
      func_lines << "    return 0;"
      func_lines << "}"

      @block_defs << struct_lines.join("\n")
      @block_defs << func_lines.join("\n")

      env_var = "_env_#{blk_id}"
      env_init = captured.map { |cv| "&#{cv[:c_name]}" }.join(", ")
      emit("#{env_name} #{env_var} = { #{env_init} };")
      return "sp_#{class_name}_#{cmname}(#{(param_list + ["(sp_block_fn)_blk_#{blk_id}", "&#{env_var}"]).join(', ')})"
    end

    def collect_captured_vars(body, block_params)
      vars = []
      seen = Set.new(block_params)
      find_captured_reads(body, seen, vars)
      vars.uniq { |v| v[:name] }
    end

    def find_captured_reads(node, seen, vars)
      return unless node
      case node
      when Prism::LocalVariableReadNode
        name = node.name.to_s
        unless seen.include?(name)
          v = lookup_var(name)
          if v
            vars << { name: name, c_name: v.c_name, type: v.type }
            seen << name
          end
        end
      when Prism::LocalVariableWriteNode
        name = node.name.to_s
        unless seen.include?(name)
          v = lookup_var(name)
          if v
            vars << { name: name, c_name: v.c_name, type: v.type }
            seen << name
          end
        end
        find_captured_reads(node.value, seen, vars)
      when Prism::LocalVariableOperatorWriteNode
        name = node.name.to_s
        unless seen.include?(name)
          v = lookup_var(name)
          if v
            vars << { name: name, c_name: v.c_name, type: v.type }
            seen << name
          end
        end
        find_captured_reads(node.value, seen, vars)
      else
        # Recurse into child nodes
        node.child_nodes.each { |c| find_captured_reads(c, seen, vars) if c }
      end
    end

    def generate_block_body_with_env(block, captured)
      body = block.body
      return unless body
      stmts = body.is_a?(Prism::StatementsNode) ? body.body : [body]
      stmts.each { |s| generate_stmt_with_env(s, captured) }
    end

    def generate_stmt_with_env(node, captured)
      # Override local variable access to use env pointers for captured vars
      case node
      when Prism::LocalVariableWriteNode
        name = node.name.to_s
        cap = captured.find { |c| c[:name] == name }
        if cap
          val = compile_expr_with_env(node.value, captured)
          emit("(*_e->#{name}) = #{val};")
        else
          val = compile_expr_with_env(node.value, captured)
          emit("#{compile_var_ref_env(name, captured)} = #{val};")
        end
      when Prism::LocalVariableOperatorWriteNode
        name = node.name.to_s
        op = node.binary_operator.to_s
        cap = captured.find { |c| c[:name] == name }
        val = compile_expr_with_env(node.value, captured)
        if cap
          emit("(*_e->#{name}) #{op}= #{val};")
        else
          emit("lv_#{name} #{op}= #{val};")
        end
      when Prism::CallNode
        mname = node.name.to_s
        if mname == "puts"
          args = call_args(node)
          args.each do |arg|
            type = infer_type(arg)
            val = compile_expr_with_env(arg, captured)
            case type
            when Type::INTEGER
              emit("printf(\"%lld\\n\", (long long)#{val});")
            when Type::STRING
              emit("{ const char *_ps = #{val}; fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }")
            else
              emit("printf(\"%lld\\n\", (long long)#{val});")
            end
          end
        else
          val = compile_expr_with_env(node, captured)
          emit("#{val};") if val && val != "" && val != "0"
        end
      when Prism::IfNode
        cond = compile_expr_with_env(node.predicate, captured)
        emit("if (#{cond}) {")
        @indent += 1
        if node.statements
          stmts = node.statements.is_a?(Prism::StatementsNode) ? node.statements.body : [node.statements]
          stmts.each { |s| generate_stmt_with_env(s, captured) }
        end
        @indent -= 1
        if node.subsequent
          case node.subsequent
          when Prism::ElseNode
            emit("else {")
            @indent += 1
            if node.subsequent.statements
              stmts = node.subsequent.statements.is_a?(Prism::StatementsNode) ? node.subsequent.statements.body : [node.subsequent.statements]
              stmts.each { |s| generate_stmt_with_env(s, captured) }
            end
            @indent -= 1
            emit("}")
          end
        else
          emit("}")
        end
      else
        generate_stmt(node)
      end
    end

    def compile_expr_with_env(node, captured)
      case node
      when Prism::LocalVariableReadNode
        name = node.name.to_s
        cap = captured.find { |c| c[:name] == name }
        if cap
          "(*_e->#{name})"
        else
          "lv_#{name}"
        end
      when Prism::CallNode
        # For env blocks, need to handle captured var references in calls
        mname = node.name.to_s
        recv = node.receiver
        args = call_args(node)

        if recv
          recv_code = compile_expr_with_env(recv, captured)
          recv_type = infer_type(recv)
          if ["+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">="].include?(mname)
            arg = compile_expr_with_env(args[0], captured)
            return "(#{recv_code} #{mname} #{arg})"
          end
        end

        # For user methods with blocks (nested yield)
        if @methods[mname] && @methods[mname].has_yield && node.block
          compiled_args = args.map { |a| compile_expr_with_env(a, captured) }
          # Need to create nested block
          return compile_nested_block_call(node, compiled_args, captured)
        elsif @methods[mname]
          compiled_args = args.map { |a| compile_expr_with_env(a, captured) }
          mi = @methods[mname]
          if mi.has_yield && !node.block
            compiled_args << "NULL"
            compiled_args << "NULL"
          end
          return "sp_#{mname}(#{compiled_args.join(', ')})"
        end

        compile_expr(node)
      else
        compile_expr(node)
      end
    end

    def compile_nested_block_call(node, compiled_args, outer_captured)
      # Handle nested yield calls inside block environments
      @needs_block_fn = true
      blk = node.block
      params = block_params(blk)
      iter_var = params[0] || "_arg"
      blk_id = next_block_id
      mname = node.name.to_s

      # For nested blocks, captured vars come from outer env
      inner_captured = collect_captured_vars(blk.body, params)

      env_name = "_blk_#{blk_id}_env"
      struct_lines = ["typedef struct { "]
      inner_captured.each do |cv|
        struct_lines[0] += "#{c_type(cv[:type])} *#{cv[:name]}; "
      end
      struct_lines[0] += "} #{env_name};"

      func_lines = []
      func_lines << "static mrb_int _blk_#{blk_id}(void *_env, mrb_int _arg) {"
      func_lines << "    #{env_name} *_e = (#{env_name} *)_env;"
      func_lines << "    mrb_int lv_#{iter_var} = _arg;"

      old_main = @in_main
      old_indent = @indent
      old_bodies = @func_bodies
      @func_bodies = []
      @in_main = false
      @indent = 1

      push_scope
      declare_var(iter_var, Type::INTEGER, c_name: "lv_#{iter_var}")
      generate_block_body_with_env(blk, inner_captured)
      pop_scope

      block_code = @func_bodies
      @func_bodies = old_bodies
      @in_main = old_main
      @indent = old_indent

      block_code.each { |l| func_lines << l }
      func_lines << "    return 0;"
      func_lines << "}"

      @block_defs << struct_lines.join("\n")
      @block_defs << func_lines.join("\n")

      # Build env initialization - reference outer captured vars via outer env
      env_var = "_env_#{blk_id}"
      env_init_parts = inner_captured.map do |cv|
        outer = outer_captured.find { |oc| oc[:name] == cv[:name] }
        if outer
          "&(*_e->#{cv[:name]})"
        else
          "&#{cv[:c_name]}"
        end
      end
      emit("#{env_name} #{env_var} = { #{env_init_parts.join(', ')} };")
      "sp_#{mname}(#{(compiled_args + ["(sp_block_fn)_blk_#{blk_id}", "&#{env_var}"]).join(', ')})"
    end

    def compile_var_ref_env(name, captured)
      cap = captured.find { |c| c[:name] == name }
      cap ? "(*_e->#{name})" : "lv_#{name}"
    end

    def block_params(block)
      return [] unless block.is_a?(Prism::BlockNode)
      return [] unless block.parameters
      params = block.parameters
      # Handle numbered parameters (_1, _2, etc.)
      if params.is_a?(Prism::NumberedParametersNode)
        return (1..params.maximum).map { |i| "_#{i}" }
      end
      if params.is_a?(Prism::BlockParametersNode)
        params = params.parameters
      end
      return [] unless params
      result = []
      (params.requireds || []).each do |p|
        case p
        when Prism::RequiredParameterNode
          result << p.name.to_s
        when Prism::BlockLocalVariableNode
          # skip
        else
          result << p.to_s
        end
      end
      result
    end

    def generate_block_body(block)
      return unless block.is_a?(Prism::BlockNode) && block.body
      stmts = block.body.is_a?(Prism::StatementsNode) ? block.body.body : [block.body]
      stmts.each { |s| generate_stmt(s) }
    end

    def compile_block_expr(block)
      return "0" unless block.is_a?(Prism::BlockNode) && block.body
      stmts = block.body.is_a?(Prism::StatementsNode) ? block.body.body : [block.body]
      if stmts.length == 1
        compile_expr(stmts.first)
      else
        stmts[0..-2].each { |s| generate_stmt(s) }
        compile_expr(stmts.last)
      end
    end

    # ---- Helper methods ----
    def call_args(node)
      return [] unless node.arguments
      node.arguments.arguments || []
    end

    def collect_locals(body)
      locals = {}
      find_locals(body, locals)
      locals
    end

    def find_locals(node, locals)
      return unless node
      case node
      when Prism::StatementsNode
        node.body.each { |s| find_locals(s, locals) }
      when Prism::LocalVariableWriteNode
        name = node.name.to_s
        unless locals[name]
          t = infer_type(node.value)
          # Promote STRING to MUTABLE_STRING if this var is used mutably (<<, setbyte, etc.)
          if t == Type::STRING && @mutable_string_vars && @mutable_string_vars.include?(name)
            t = Type::MUTABLE_STRING
            @needs_mutable_string = true
          end
          locals[name] = t
          # Register in current scope so subsequent infer_type calls can resolve
          existing = lookup_var(name)
          if !existing && t != Type::UNKNOWN
            declare_var(name, t, c_name: "lv_#{name}")
          end
        end
        find_locals(node.value, locals)
      when Prism::LocalVariableOperatorWriteNode
        val_type = infer_type(node.value)
        # If the value is a local var read, check method params and already-found locals
        if val_type == Type::UNKNOWN && node.value.is_a?(Prism::LocalVariableReadNode)
          ref_name = node.value.name.to_s
          val_type = locals[ref_name] if locals[ref_name]
          if val_type == Type::UNKNOWN && @current_method
            param = @current_method.params.find { |p| p.name == ref_name }
            val_type = param.type if param && param.type != Type::UNKNOWN
          end
        end
        existing = locals[node.name.to_s]
        if existing == Type::INTEGER && val_type == Type::FLOAT
          locals[node.name.to_s] = Type::FLOAT
        else
          locals[node.name.to_s] ||= (val_type == Type::FLOAT ? Type::FLOAT : Type::INTEGER)
        end
      when Prism::IfNode
        find_locals(node.statements, locals)
        find_locals(node.subsequent, locals)
      when Prism::ElseNode
        find_locals(node.statements, locals)
      when Prism::WhileNode
        find_locals(node.statements, locals)
      when Prism::ForNode
        find_locals(node.statements, locals)
      when Prism::CaseNode
        (node.conditions || []).each { |c| find_locals(c, locals) if c.respond_to?(:statements) }
        find_locals(node.else_clause, locals) if node.else_clause
      when Prism::WhenNode
        find_locals(node.statements, locals)
      when Prism::BeginNode
        find_locals(node.statements, locals)
        find_locals(node.rescue_clause, locals) if node.rescue_clause
      when Prism::RescueNode
        find_locals(node.statements, locals)
        if node.reference
          name = case node.reference
                 when Prism::LocalVariableTargetNode then node.reference.name.to_s
                 else nil
                 end
          locals[name] = Type::STRING if name
        end
      when Prism::CallNode
        if node.block
          find_locals(node.block, locals)
        end
      when Prism::BlockNode
        find_locals(node.body, locals)
      end
    end

    def declare_locals_from_body(body)
      locals = collect_locals(body)
      locals.each do |name, type|
        next if @current_method && @current_method.params.any? { |p| p.name == name }
        # Check if this is a class-typed array variable
        elem_class = @array_elem_types[name]
        arr_size = @local_array_sizes[name]
        if elem_class && arr_size
          eci = @classes[elem_class]
          declare_var(name, elem_class, c_name: "lv_#{name}")
          if eci && class_needs_gc?(eci)
            emit("sp_#{elem_class} *lv_#{name}[#{arr_size}];")
          else
            emit("sp_#{elem_class} lv_#{name}[#{arr_size}];")
          end
        else
          declare_var(name, type, c_name: "lv_#{name}")
          emit("#{c_type(type)} lv_#{name} = #{default_val(type)};")
          # Add GC root for pointer-type locals (class instances that need GC)
          if type.is_a?(String) && @classes[type] && class_needs_gc?(@classes[type])
            @needs_gc = true
            emit("SP_GC_ROOT(lv_#{name});")
          end
        end
      end
    end

    def last_stmt(body)
      return body unless body.is_a?(Prism::StatementsNode)
      body.body.last
    end

    def returns_value?(node)
      return false unless node
      case node
      when Prism::ReturnNode
        true
      when Prism::IfNode
        # Check if all branches return
        false  # conservative
      else
        false
      end
    end

    def const_value_to_c(node, type)
      case node
      when Prism::IntegerNode then node.value.to_s
      when Prism::FloatNode
        s = node.value.to_s
        s = s + ".0" unless s.include?('.') || s.include?('e') || s.include?('E')
        s
      when Prism::StringNode then c_string_literal(node.respond_to?(:unescaped) ? node.unescaped : node.content)
      when Prism::TrueNode then "TRUE"
      when Prism::FalseNode then "FALSE"
      when Prism::CallNode
        # Handle simple constant expressions like 1 << 29, BNUM.to_f
        mname = node.name.to_s
        if %w[<< >> + - * / % & | ^].include?(mname) && node.receiver && node.arguments
          left = const_value_to_c(node.receiver, type)
          right = const_value_to_c(node.arguments.arguments.first, type)
          "(#{left} #{mname} #{right})"
        elsif mname == "to_f" && node.receiver
          recv = const_value_to_c(node.receiver, type)
          "((mrb_float)(#{recv}))"
        elsif mname == "to_i" && node.receiver
          recv = const_value_to_c(node.receiver, type)
          "((mrb_int)(#{recv}))"
        else
          "0"
        end
      when Prism::ConstantReadNode
        name = node.name.to_s
        if @constants[name]
          "cv_#{name}"
        elsif @module_constants
          # Check all module constants
          @module_constants.each do |path, info|
            if path.end_with?("::#{name}")
              return info[:c_name]
            end
          end
          "0"
        else
          "0"
        end
      else "0"
      end
    end

    def c_string_literal(s)
      # Prism gives us the actual string value (unescaped).
      # We need to escape it for a C string literal.
      # Order matters: escape backslashes first, then other chars
      # Use block form for gsub to avoid backslash replacement issues
      escaped = s.gsub("\\") { "\\\\" }  # \ -> \\
      escaped = escaped.gsub('"') { "\\\"" }  # " -> \"
      escaped = escaped.gsub("\n") { "\\n" }  # newline -> \n
      escaped = escaped.gsub("\t") { "\\t" }  # tab -> \t
      escaped = escaped.gsub("\r") { "\\r" }  # CR -> \r
      escaped = escaped.gsub("\0") { "\\0" }  # null -> \0
      "\"#{escaped}\""
    end

    def c_type(type)
      case type
      when Type::INTEGER then "mrb_int"
      when Type::FLOAT then "mrb_float"
      when Type::BOOLEAN then "mrb_bool"
      when Type::STRING, Type::SYMBOL then "const char *"
      when Type::NIL then "mrb_int"
      when Type::ARRAY then "sp_IntArray *"
      when Type::FLOAT_ARRAY then "sp_FloatArray *"
      when Type::STR_ARRAY then "sp_StrArray *"
      when Type::HASH then "sp_StrIntHash *"
      when Type::STR_HASH then "sp_RbHash *"
      when Type::POLY_ARRAY then "sp_IntArray *"  # reuse IntArray, values are boxed sp_RbValue
      when Type::POLY_HASH then "sp_PolyHash *"
      when Type::RANGE then "sp_Range"
      when Type::MUTABLE_STRING then "sp_String *"
      when Type::TIME then "sp_Time"
      when Type::FILE_OBJ then "FILE *"
      when Type::STRINGIO then "sp_StringIO *"
      when Type::PROC then "sp_Proc *"
      when Type::POLY then "sp_RbValue"
      when Type::VOID then "void"
      else
        # Check if it's a class instance type (string class name)
        if type.is_a?(String) && @classes[type]
          ci = @classes[type]
          if class_needs_gc?(ci)
            "sp_#{type} *"
          else
            "sp_#{type}"
          end
        else
          "mrb_int"
        end
      end
    end

    def default_val(type)
      case type
      when Type::INTEGER then "0"
      when Type::FLOAT then "0.0"
      when Type::BOOLEAN then "FALSE"
      when Type::STRING, Type::SYMBOL then "NULL"
      when Type::NIL then "0"
      when Type::ARRAY then "NULL"
      when Type::FLOAT_ARRAY then "NULL"
      when Type::STR_ARRAY then "NULL"
      when Type::HASH then "NULL"
      when Type::STR_HASH then "NULL"
      when Type::POLY_ARRAY then "NULL"
      when Type::POLY_HASH then "NULL"
      when Type::RANGE then "{0,0}"
      when Type::TIME then "{0}"
      when Type::MUTABLE_STRING then "NULL"
      when Type::STRINGIO then "NULL"
      when Type::PROC then "NULL"
      when Type::POLY then "sp_box_nil()"
      when Type::VOID then ""
      else
        if type.is_a?(String) && @classes[type]
          ci = @classes[type]
          class_needs_gc?(ci) ? "NULL" : "{0}"
        else
          "0"
        end
      end
    end

    def gc_type?(type)
      [Type::ARRAY, Type::FLOAT_ARRAY, Type::STR_ARRAY, Type::POLY_ARRAY, Type::HASH, Type::STR_HASH, Type::POLY_HASH, Type::MUTABLE_STRING].include?(type) ||
        (type.is_a?(String) && @classes[type])
    end

    # ---- Phase 4: Assembly ----
    def assemble_output
      out = StringIO.new

      # Header
      out.puts "/* Generated by Spinel AOT compiler */"
      out.puts "/* Compile: cc -O2 <this file> -lm -o <output> */"
      out.puts "#include <stdio.h>"
      out.puts "#include <stdlib.h>"
      out.puts "#include <string.h>"
      out.puts "#include <math.h>"
      out.puts "#include <stdbool.h>"
      out.puts "#include <stdint.h>"
      out.puts "#include <ctype.h>"
      out.puts "#include <unistd.h>"
      out.puts "#include <signal.h>"
      out.puts "#include <stdarg.h>"
      out.puts "#include <libgen.h>"
      out.puts "#include <glob.h>"
      out.puts "#include <sys/stat.h>"
      out.puts

      # Type definitions
      out.puts "typedef int64_t mrb_int;"
      out.puts "typedef double mrb_float;"
      out.puts "typedef bool mrb_bool;"
      out.puts "#ifndef TRUE"
      out.puts "#define TRUE true"
      out.puts "#endif"
      out.puts "#ifndef FALSE"
      out.puts "#define FALSE false"
      out.puts "#endif"
      out.puts
      # Ruby-compatible floor division and modulo for integers
      out.puts "static inline mrb_int sp_idiv(mrb_int a, mrb_int b) {"
      out.puts "  mrb_int q = a / b;"
      out.puts "  mrb_int r = a % b;"
      out.puts "  if ((r != 0) && ((r ^ b) < 0)) q--;"
      out.puts "  return q;"
      out.puts "}"
      out.puts "static inline mrb_int sp_imod(mrb_int a, mrb_int b) {"
      out.puts "  mrb_int r = a % b;"
      out.puts "  if ((r != 0) && ((r ^ b) < 0)) r += b;"
      out.puts "  return r;"
      out.puts "}"
      out.puts
      # Ruby-compatible putc_utf8 for printf("%c", int) - matches CRuby UTF-8 output
      out.puts "static inline void sp_putc_utf8(mrb_int c) {"
      out.puts "  if (c < 0x80) { putchar((int)c); }"
      out.puts "  else if (c < 0x800) { putchar(0xC0 | (c >> 6)); putchar(0x80 | (c & 0x3F)); }"
      out.puts "  else if (c < 0x10000) { putchar(0xE0 | (c >> 12)); putchar(0x80 | ((c >> 6) & 0x3F)); putchar(0x80 | (c & 0x3F)); }"
      out.puts "  else { putchar(0xF0 | (c >> 18)); putchar(0x80 | ((c >> 12) & 0x3F)); putchar(0x80 | ((c >> 6) & 0x3F)); putchar(0x80 | (c & 0x3F)); }"
      out.puts "}"
      out.puts

      # Auto-dependencies (order matters: propagate from high-level to low-level)
      @needs_str_int_hash = true if @needs_str_str_hash  # RbHash uses sp_hash_str from StrIntHash
      @needs_str_int_hash = true if @needs_poly_hash     # PolyHash uses sp_hash_str from StrIntHash
      @needs_poly = true if @needs_poly_hash             # PolyHash values are sp_RbValue
      @needs_int_array = true if @needs_str_int_hash     # StrIntHash_values needs IntArray
      @needs_gc = true if @needs_int_array || @needs_float_array || @needs_str_int_hash
      @needs_gc = true if @needs_str_str_hash
      @needs_gc = true if @needs_poly_hash
      @needs_gc = true if @needs_mutable_string
      @needs_str_array = true if @string_helpers_needed.include?(:str_split) || @string_helpers_needed.include?(:str_chars)

      # String helpers (non-StrArray dependent)
      emit_string_helpers(out, before_str_array: true)

      # Mutable String (before GC since it uses malloc)
      emit_mutable_string(out) if @needs_mutable_string

      # Exception handling
      emit_exception_runtime(out) if @needs_exception

      # GC runtime
      emit_gc_runtime(out) if @needs_gc

      # IntArray
      emit_int_array(out) if @needs_int_array

      # FloatArray
      emit_float_array(out) if @needs_float_array

      # Range (standalone, if int_array wasn't needed)
      emit_range(out) if @needs_range

      # StrArray
      emit_str_array(out) if @needs_str_array

      # String helpers (StrArray dependent - split, chars)
      emit_string_helpers(out, before_str_array: false)

      # StrIntHash
      emit_str_int_hash(out) if @needs_str_int_hash

      # StrStrHash (RbHash)
      emit_str_str_hash(out) if @needs_str_str_hash

      # NaN-boxed polymorphic value runtime
      emit_poly_runtime(out) if @needs_poly

      # PolyHash (string -> sp_RbValue)
      emit_poly_hash(out) if @needs_poly_hash

      out.puts "static int sp_last_status = 0;"
      out.puts

      # Time runtime
      emit_time_runtime(out) if @needs_time

      # File runtime
      emit_file_runtime(out) if @needs_file

      # StringIO runtime
      emit_stringio_runtime(out) if @needs_stringio

      # System runtime
      emit_system_runtime(out) if @needs_system

      # Regexp runtime (oniguruma)
      emit_regexp_runtime(out) if @needs_regexp

      # Constants (global)
      @constants.each do |name, info|
        out.puts "static #{c_type(info[:type])} cv_#{name} = #{default_val(info[:type])};"
      end

      # Module constants
      @module_constants.each do |path, info|
        out.puts "#define #{info[:c_name]} (#{info[:value]})"
      end

      # Module ivars (static variables)
      if @module_ivars
        @module_ivars.each do |mod_name, ivars|
          ivars.each do |ivar, info|
            ct = c_type(info[:type])
            dv = default_val(info[:type])
            val = const_value_to_c(info[:value_node], info[:type])
            out.puts "static #{ct} sp_#{mod_name}_#{ivar} = #{val};"
          end
        end
      end
      out.puts

      # Class structs
      @struct_decls.each { |s| out.puts s; out.puts }

      # Block type
      if @needs_block_fn
        out.puts "typedef mrb_int (*sp_block_fn)(void *env, mrb_int arg);"
        out.puts
      end

      # Proc runtime
      if @needs_proc
        out.puts <<~C
          /* ---- sp_Proc runtime ---- */
          typedef struct { sp_block_fn fn; void *env; } sp_Proc;
          static sp_Proc *sp_Proc_new(sp_block_fn fn, void *env) {
            sp_Proc *p = (sp_Proc *)malloc(sizeof(sp_Proc));
            p->fn = fn; p->env = env;
            return p;
          }
          static mrb_int sp_Proc_call(sp_Proc *p, mrb_int arg) {
            return p->fn(p->env, arg);
          }
        C
        out.puts
      end

      # Forward declarations
      @forward_decls.each { |f| out.puts f }
      out.puts

      # Block definitions
      @block_defs.each { |b| out.puts b; out.puts }

      # Function bodies
      @func_bodies.each { |f| out.puts f; out.puts }

      # ARGV support
      out.puts "/* ARGV support */"
      out.puts "typedef struct { const char **data; mrb_int len; } sp_Argv;"
      out.puts "static sp_Argv sp_argv;"
      out.puts "static mrb_int sp_Argv_length(sp_Argv *a) { return a->len; }"
      out.puts
      out.puts "static const char *sp_program_name = \"\";"

      # Main
      out.puts "int main(int argc, char **argv) {"
      out.puts "  sp_program_name = argv[0];"
      out.puts "  sp_argv.data = (const char **)(argv + 1); sp_argv.len = argc - 1;"

      # Main variable declarations
      vol = @needs_exception ? "volatile " : ""
      @main_vars.each do |mv|
        type = mv[:type]
        name = mv[:name]
        cname = mv[:class_name]
        if cname && @classes[cname]
          ci = @classes[cname]
          needs_ptr = class_needs_gc?(ci)
          has_heap = ci.ivars.any? { |_k, v| [Type::ARRAY, Type::STR_ARRAY, Type::HASH].include?(v) }
          if needs_ptr || has_heap
            out.puts "  sp_#{cname} *#{name} = NULL;"
            out.puts "  SP_GC_ROOT(#{name});"
          else
            out.puts "  sp_#{cname} #{name};"
          end
        elsif gc_type?(type)
          out.puts "  #{c_type(type)} #{name} = #{default_val(type)};"
          out.puts "  SP_GC_ROOT(#{name});" if @needs_gc
        elsif type == Type::RANGE
          out.puts "  #{c_type(type)} #{name};"
        else
          out.puts "  #{vol}#{c_type(type)} #{name} = #{default_val(type)};"
        end
      end
      out.puts

      # Regexp initialization
      out.puts "  sp_regexp_init();" if @needs_regexp
      out.puts

      # Constants initialization
      @constants.each do |name, info|
        val = const_value_to_c(info[:node], info[:type])
        out.puts "  cv_#{name} = #{val};"
      end

      # Main body
      @main_body.each { |line| out.puts line }
      out.puts
      out.puts "  return 0;"
      out.puts "}"

      result = out.string
      # Post-process: fix mrb_int -> pointer dereference errors (inline calls only)
      result = fix_mrb_int_dereference(result)
      # Post-process: fix _cres_N = return expr; -> return expr;
      result = fix_return_in_expression(result)
      # Post-process: fix stray backslashes in string literals
      result = fix_stray_backslashes(result)
      # Post-process: fix undeclared _ary_N/_hsh_N in constructors
      result = fix_undeclared_literals(result)
      # Post-process: fix string += compiled as C += instead of sp_str_concat
      result = fix_string_concat_ops(result)
      # Post-process: fix undeclared block variables
      result = fix_undeclared_block_vars(result)
      # Post-process: fix IntArray_push with struct/pointer arguments
      result = fix_intarray_push_cast(result)
      # Post-process: fix dot-member-access on mrb_int (e.g. lv_v.c_name)
      result = fix_dot_member_on_mrb_int(result)
      # Post-process: fix sp_IntArray_get passed to functions expecting pointer args
      result = fix_intarray_get_ptr_arg(result)
      # Post-process: fix mrb_int variable -> pointer dereferences (must run after
      # fix_undeclared_block_vars which adds missing mrb_int declarations)
      result = fix_mrb_int_deref_variables(result)
      result
    end

    # Find the balanced closing paren for a function call and fix ->field dereference
    def fix_hash_get_deref(line, func_name, struct_fields)
      result = ""
      remaining = line
      while (idx = remaining.index(func_name))
        result << remaining[0...idx]
        remaining = remaining[idx..]
        # Find the opening paren
        open_idx = func_name.length
        if remaining[open_idx] == "("
          # Find matching close paren using balanced counting
          depth = 1
          pos = open_idx + 1
          while pos < remaining.length && depth > 0
            depth += 1 if remaining[pos] == "("
            depth -= 1 if remaining[pos] == ")"
            pos += 1
          end
          # pos is now right after the closing paren
          call_text = remaining[0...pos]  # e.g. "sp_StrIntHash_get(...)"
          args_text = remaining[(open_idx + 1)...(pos - 1)]  # the args inside parens
          after = remaining[pos..]
          if after =~ /\A->(\w+)(.*)/
            field = $1
            rest = $2
            if struct_fields[field] && struct_fields[field].length > 0
              stype = struct_fields[field][0]
              result << "((sp_#{stype} *)#{call_text})->#{field}"
            else
              result << "((sp_SpNode *)#{call_text})->#{field}"
            end
            remaining = rest
          else
            result << call_text
            remaining = after
          end
        else
          result << func_name
          remaining = remaining[func_name.length..]
        end
      end
      result << remaining
      result
    end

    def fix_mrb_int_dereference(code)
      # Fix patterns where sp_StrIntHash_get or sp_IntArray_get results are dereferenced
      # Pattern: sp_StrIntHash_get(X, Y)->field  =>  ((void *)sp_StrIntHash_get(X, Y))->field
      # Also: _he_N->value)->field when value is mrb_int
      # Use regex to find and fix

      # Build a map of struct field names to their owning struct types
      struct_fields = {}
      @classes.each do |cname, ci|
        all_ivars = collect_all_ivars(ci)
        all_ivars.each do |iname, _itype|
          struct_fields[iname] ||= []
          struct_fields[iname] << cname
        end
        (ci.attrs[:reader] || []).each { |a| struct_fields[a] ||= []; struct_fields[a] << cname }
        (ci.attrs[:accessor] || []).each { |a| struct_fields[a] ||= []; struct_fields[a] << cname }
      end
      # Add special mappings for built-in types
      # Hash field ->first means the variable is a hash pointer
      # We handle these specially in the cast logic below
      # De-duplicate struct_fields entries
      struct_fields.each { |k, v| struct_fields[k] = v.uniq }

      lines = code.split("\n")
      fixed_lines = lines.map do |line|
        # Pattern 1: sp_StrIntHash_get(...)->field - use balanced paren matching
        line = fix_hash_get_deref(line, "sp_StrIntHash_get", struct_fields)
        # Pattern 2: sp_IntArray_get(...)->field
        line = fix_hash_get_deref(line, "sp_IntArray_get", struct_fields)
        line
      end

      # Pattern 3: local variables declared as mrb_int but used with ->
      # Track variables assigned from sp_StrIntHash_get or sp_IntArray_get (including indirect via _he_N->value)
      # Then cast those variables when they're used with ->
      result = fixed_lines.join("\n")

      # Find all variables that need casting: assigned from hash/array get and used with ->
      # Step 1: find variables assigned from hash_get/array_get or hash entry values
      hash_vars = {}  # var_name => true
      result.scan(/mrb_int\s+(lv_\w+)\s*=\s*(?:sp_StrIntHash_get|sp_IntArray_get)\(/) do
        hash_vars[$1] = true
      end
      # Also: lv_X = _he_N->value where _he is a hash entry iterator
      result.scan(/(lv_\w+)\s*=\s*_he_\d+->value/) do
        hash_vars[$1] = true
      end
      # Also: lv_X = sp_StrIntHash_get(...) (non-declaration assignment)
      result.scan(/(lv_\w+)\s*=\s*(?:sp_StrIntHash_get|sp_IntArray_get)\(/) do
        hash_vars[$1] = true
      end

      result
    end

    # Separate pass for fixing mrb_int variable dereferences.  This runs
    # AFTER fix_undeclared_block_vars so that all variable declarations
    # are present in the code.
    def fix_mrb_int_deref_variables(code)
      struct_fields = {}
      @classes.each do |cname, ci|
        all_ivars = collect_all_ivars(ci)
        all_ivars.each do |iname, _itype|
          struct_fields[iname] ||= []
          struct_fields[iname] << cname
        end
        (ci.attrs[:reader] || []).each { |a| struct_fields[a] ||= []; struct_fields[a] << cname }
        (ci.attrs[:accessor] || []).each { |a| struct_fields[a] ||= []; struct_fields[a] << cname }
      end
      struct_fields.each { |k, v| struct_fields[k] = v.uniq }
      hash_fields = %w[first last cap size buckets]
      fix_mrb_int_deref_per_function(code, struct_fields, hash_fields)
    end

    # Apply mrb_int -> pointer casts using line-by-line nearest-declaration
    # lookup.  For each lv_X->field, we find the nearest preceding
    # declaration of lv_X.  If it is mrb_int, we add a cast; if it is a
    # typed pointer, we leave it alone.  This avoids the fragile problem
    # of trying to detect C function boundaries.
    def fix_mrb_int_deref_per_function(code, struct_fields, hash_fields)
      lines = code.split("\n")

      # Pass 1: record every declaration of lv_* variables with their type.
      # var_decls[var_name] = [[line_no, :mrb_int/:typed], ...]  sorted by line
      var_decls = {}
      lines.each_with_index do |line, i|
        line.scan(/mrb_int\s+(lv_\w+)\s*[=;,)]/) do
          (var_decls[$1] ||= []) << [i, :mrb_int]
        end
        line.scan(/(?:sp_\w+|const\s+char)\s*\*\s*(lv_\w+)/) do
          (var_decls[$1] ||= []) << [i, :typed]
        end
      end

      # Also record function-start lines for same-function checks
      func_boundary_lines = Set.new
      lines.each_with_index do |l, i|
        if l =~ /\Astatic\s+/ && l.include?("(") && l.rstrip.end_with?("{")
          func_boundary_lines << i
        end
      end
      # Precompute: for each line, the last function boundary at or before it
      last_func_start = Array.new(lines.length, -1)
      cur = -1
      lines.each_index do |i|
        cur = i if func_boundary_lines.include?(i)
        last_func_start[i] = cur
      end


      # Pass 2: process each line that has uncast lv_X->field
      lines.each_with_index do |line, i|
        next unless line =~ /\blv_\w+->/ && line !~ /\A\s*(?:static\s|\/\*|\/\/|#define)/
        my_func = last_func_start[i]
        lines[i] = line.gsub(/(?<!\*\))\b(lv_\w+)->(\w+)/) do
          var = $1; field = $2
          decls = var_decls[var]
          if decls
            # Find nearest declaration in the SAME function
            nearest = decls.select { |ln, _| ln <= i && last_func_start[ln] == my_func }.last
            if nearest && nearest[1] == :mrb_int
              cast = resolve_deref_cast(var, field, i, lines, struct_fields, hash_fields)
              "((sp_#{cast} *)#{var})->#{field}"
            else
              "#{var}->#{field}"
            end
          else
            "#{var}->#{field}"
          end
        end
      end

      lines.join("\n")
    end

    # Determine the best cast type for a mrb_int variable dereferencing a field.
    def resolve_deref_cast(var, field, line_no, lines, struct_fields, hash_fields)
      return "StrIntHash" if hash_fields.include?(field)

      # Look at context around the variable (backwards ~300 lines to its decl)
      search_start = [line_no - 300, 0].max
      context = lines[search_start..line_no].join("\n")

      # 1. Existing explicit cast on this variable in nearby context
      if context =~ /\(\(sp_(\w+)\s*\*\)#{Regexp.escape(var)}\)->/
        return $1
      end

      # 2. Unique field mapping (field exists in only one struct)
      if struct_fields[field] && struct_fields[field].length == 1
        return struct_fields[field][0]
      end

      # 3. Hash iteration assignment context
      if context =~ /#{Regexp.escape(var)}\s*=\s*(_he_\d+)->value/
        he_var = $1
        return "MethodInfo" if context =~ /#{Regexp.escape(he_var)}\b.*->methods->first/
        return "ClassInfo"  if context =~ /#{Regexp.escape(he_var)}\b.*->classes->first/
      end

      # 4. StrIntHash_get assignment
      return "MethodInfo" if context =~ /#{Regexp.escape(var)}\s*=\s*sp_StrIntHash_get\([^,]*->methods\b/
      return "ClassInfo"  if context =~ /#{Regexp.escape(var)}\s*=\s*sp_StrIntHash_get\([^,]*->classes\b/

      # 5. Variable name heuristics
      case var
      when "lv_mi" then return "MethodInfo"
      when "lv_ci" then return "ClassInfo"
      when "lv_pi" then return "ParamInfo"
      when "lv_vi", "lv_var_info" then return "VarInfo"
      end

      # 6. Fall back to struct_fields first entry or SpNode
      return struct_fields[field][0] if struct_fields[field] && !struct_fields[field].empty?
      "SpNode"
    end

    def fix_return_in_expression(code)
      # Fix pattern: _cres_N = return expr;  ->  return expr;
      code = code.gsub(/\b_cres_\d+\s*=\s*return\b/, 'return')
      # Fix pattern: return return expr;  ->  return expr;
      code = code.gsub(/\breturn\s+return\b/, 'return')
      # Fix pattern: void _cres_N = ;  ->  /* void case */
      code = code.gsub(/\bvoid\s+_cres_\d+\s*=\s*;/, '/* void case result */;')
      # Fix pattern: _cres_N = sp_void_func(...);  where the func returns void
      # This is harder to detect, so we'll let void value errors be handled by adding nil
      code
    end

    def fix_stray_backslashes(code)
      code
    end

    def fix_string_concat_ops(code)
      # Fix patterns where string += is compiled as lv_X += "..." instead of sp_str_concat
      # Pattern: lv_X += "string" or lv_X += sp_str_concat(...)
      # These should be: lv_X = sp_str_concat(lv_X, ...)
      lines = code.split("\n")
      fixed = lines.map do |line|
        # Pattern: lv_var += expr; where lv_var is const char *
        if line =~ /\A(\s*)(lv_\w+)\s*\+=\s*(.+);$/
          indent = $1
          var = $2
          expr = $3
          # Check if this is likely a string concat (not numeric +=)
          if expr =~ /^"/ || expr =~ /^sp_str_concat/ || expr =~ /^sp_int_to_s/
            "#{indent}#{var} = sp_str_concat(#{var}, #{expr});"
          else
            line
          end
        else
          line
        end
      end
      fixed.join("\n")
    end

    def fix_undeclared_block_vars(code)
      # Two-pass approach:
      # Pass 1: Identify functions and their variable declarations
      # Pass 2: Add missing declarations

      lines = code.split("\n")

      # Pass 1: Find function boundaries and collect declared/used vars
      functions = []  # [{start:, end:, declared: {}, used: {}}]
      brace_depth = 0
      current_func = nil

      lines.each_with_index do |line, idx|
        # Strip string literals before counting braces to avoid miscounts
        stripped_for_braces = line.gsub(/"(?:[^"\\]|\\.)*"/, '')
        if line =~ /^static\s+\S+/ && line.include?("{")
          current_func = { start: idx, declared: {}, used: {} }
          brace_depth = stripped_for_braces.count("{") - stripped_for_braces.count("}")
        elsif current_func
          brace_depth += stripped_for_braces.count("{") - stripped_for_braces.count("}")
          if brace_depth <= 0
            current_func[:end_idx] = idx
            functions << current_func
            current_func = nil
            brace_depth = 0
          end
        end

        next unless current_func

        # Track declarations only at function body level (brace_depth == 1)
        # Declarations at deeper brace levels are block-scoped in C
        if brace_depth == 1
          line.scan(/(?:mrb_int|const char \*|mrb_float|mrb_bool|sp_\w+\s*\*?|void)\s+((?:lv_|_cres_)\w+)/) do |m|
            current_func[:declared][m[0]] = true
          end
        end
        # Also track function parameter declarations (on the start line)
        if idx == current_func[:start]
          line.scan(/(?:mrb_int|const char \*|mrb_float|mrb_bool|sp_\w+\s*\*?)\s+(lv_\w+)/) do |m|
            current_func[:declared][m[0]] = true
          end
        end
        # Track uses (excluding string literals)
        stripped = line.gsub(/"(?:[^"\\]|\\.)*"/, '')
        stripped.scan(/\b((?:lv_|_cres_)\w+)\b/) do |m|
          current_func[:used][m[0]] = true
        end
      end

      # Pass 2: Add declarations for undeclared vars
      # Process in reverse to maintain line indices
      functions.reverse.each do |func|
        undeclared = func[:used].keys.select { |v| !func[:declared][v] }
        next if undeclared.empty?

        # Infer type from usage: if assigned from _he_N->key it's const char*
        # if assigned from _he_N->value it stays mrb_int
        decl_line = undeclared.map { |v|
          # Check if this var is assigned from a hash entry key
          is_key = false
          is_str = false
          (func[:start]..([func[:end_idx] || lines.length - 1, lines.length - 1].min)).each do |li|
            l = lines[li]
            if l && l.include?("#{v} = ") && l.include?("->key")
              is_key = true
            end
            if l && l.include?("#{v} = ") && (l.include?('"') || l.include?("sp_str_"))
              is_str = true
            end
          end
          if is_key || is_str
            "  const char *#{v} = NULL;"
          else
            "  mrb_int #{v} = 0;"
          end
        }.join("\n")
        # Insert after the function opening line
        lines.insert(func[:start] + 1, decl_line)
      end

      lines.join("\n")
    end

    def fix_undeclared_literals(code)
      # Fix _ary_N and _hsh_N references in constructor functions that should be
      # sp_IntArray_new() and sp_StrIntHash_new() respectively
      code = code.gsub(/(\bself->\w+\s*=\s*)(_ary_\d+)\s*;/) do
        "#{$1}sp_IntArray_new();"
      end
      code = code.gsub(/(\bself->\w+\s*=\s*)(_hsh_\d+)\s*;/) do
        "#{$1}sp_StrIntHash_new();"
      end
      code
    end

    def fix_intarray_push_cast(code)
      # Fix sp_IntArray_push(arr, sp_make_param_info(...)) and sp_make_var_info(...)
      # These return structs by value but IntArray stores mrb_int.
      # We need to heap-allocate the struct and push the pointer.
      # Use balanced paren matching to handle nested calls
      {"sp_make_param_info" => "sp_ParamInfo", "sp_make_var_info" => "sp_VarInfo"}.each do |func, stype|
        result = ""
        remaining = code
        pattern = "sp_IntArray_push("
        while (idx = remaining.index(pattern))
          result << remaining[0...idx]
          remaining = remaining[idx..]
          # Find the balanced close paren of sp_IntArray_push(...)
          push_open = pattern.length - 1  # index of '('
          depth = 1
          pos = push_open + 1
          while pos < remaining.length && depth > 0
            depth += 1 if remaining[pos] == "("
            depth -= 1 if remaining[pos] == ")"
            pos += 1
          end
          push_call = remaining[0...pos]
          after = remaining[pos..]
          # Check if the second argument is sp_make_param_info/sp_make_var_info
          if push_call.include?(func)
            # Find the func call within the push args and wrap it
            func_idx = push_call.index(func)
            if func_idx
              # Find balanced parens for the inner func call
              inner_open = func_idx + func.length
              if push_call[inner_open] == "("
                d2 = 1
                p2 = inner_open + 1
                while p2 < push_call.length && d2 > 0
                  d2 += 1 if push_call[p2] == "("
                  d2 -= 1 if push_call[p2] == ")"
                  p2 += 1
                end
                inner_call = push_call[func_idx...p2]
                # Heap-allocate the struct and cast to mrb_int
                # { Type _t = make_func(...); sp_IntArray_push(arr, (mrb_int)memcpy(malloc(sizeof(Type)), &_t, sizeof(Type))); }
                # Extract array argument (first arg to push)
                arr_arg = push_call[pattern.length...func_idx].strip
                arr_arg = arr_arg.chomp(",").strip
                push_call = "{ #{stype} _sv = #{inner_call}; sp_IntArray_push(#{arr_arg}, (mrb_int)(intptr_t)memcpy(malloc(sizeof(#{stype})), &_sv, sizeof(#{stype}))); }"
              end
            end
          end
          result << push_call
          remaining = after
        end
        result << remaining
        code = result
      end
      code
    end

    def fix_dot_member_on_mrb_int(code)
      # Fix patterns like lv_v.c_name where lv_v is mrb_int
      # These should be ((sp_VarInfo *)lv_v)->c_name or similar
      # Collect all mrb_int local variables
      mrb_int_vars = {}
      code.scan(/mrb_int\s+(lv_\w+)\s*[=;,)]/) { mrb_int_vars[$1] = true }

      # Build a map of struct fields
      struct_fields = {}
      @classes.each do |cname, ci|
        all_ivars = collect_all_ivars(ci)
        all_ivars.each do |iname, _itype|
          struct_fields[iname] ||= []
          struct_fields[iname] << cname
        end
        (ci.attrs[:reader] || []).each { |a| struct_fields[a] ||= []; struct_fields[a] << cname }
        (ci.attrs[:accessor] || []).each { |a| struct_fields[a] ||= []; struct_fields[a] << cname }
      end

      mrb_int_vars.each_key do |var|
        # Fix var.field (dot access on mrb_int) -> ((type *)var)->field
        code = code.gsub(/\b#{Regexp.escape(var)}\.(\w+)\b/) do
          field = $1
          if struct_fields[field] && struct_fields[field].length > 0
            stype = struct_fields[field][0]
            "((sp_#{stype} *)#{var})->#{field}"
          else
            "((sp_VarInfo *)#{var})->#{field}"
          end
        end
      end
      code
    end

    def fix_intarray_get_ptr_arg(code)
      # Fix sp_IntArray_get(...) passed as argument to functions expecting pointer types.
      # sp_IntArray_get returns mrb_int, but functions like sp_set_node_field_* expect
      # sp_SpNode * or sp_IntArray * arguments.  We wrap with (Type *)(intptr_t) cast.
      #
      # Patterns handled (balanced-paren aware):
      #   sp_set_node_field_{string,int,float,ref}( sp_IntArray_get(...), ... )
      #     -> first arg is sp_SpNode *
      #   sp_set_node_field_ref( ..., sp_IntArray_get(...) )
      #     -> third arg is sp_SpNode *
      #   sp_set_node_field_array( sp_IntArray_get(...), ..., arr )
      #     -> first arg is sp_SpNode *
      #   sp_SpNode_*( sp_IntArray_get(...), ... )
      #     -> first arg is sp_SpNode *
      #   sp_SpNode__add_child( sp_IntArray_get(...), ... )
      #     -> first arg is sp_SpNode *
      #   sp_SpNode__add_children( sp_IntArray_get(...), ..., sp_IntArray_get(...) )
      #     -> first arg is sp_SpNode *, third arg stays (it's sp_IntArray * but stored in IntArray)

      # Map: function name prefix -> { arg_positions => cast_type }
      # We use a line-based approach with balanced paren matching.
      lines = code.split("\n")
      fixed = lines.map do |line|
        # Pattern: sp_set_node_field_*(sp_IntArray_get(...), ...)
        # Cast first arg of sp_set_node_field_* to (sp_SpNode *)(intptr_t)
        %w[sp_set_node_field_string sp_set_node_field_int
           sp_set_node_field_float sp_set_node_field_ref
           sp_set_node_field_array].each do |fname|
          line = cast_intarray_get_in_func_arg(line, fname, 0, "sp_SpNode *")
        end

        # Pattern: sp_set_node_field_ref(..., ..., sp_IntArray_get(...))
        # Cast third arg (index 2) to (sp_SpNode *)(intptr_t)
        line = cast_intarray_get_in_func_arg(line, "sp_set_node_field_ref", 2, "sp_SpNode *")

        # Pattern: sp_set_node_field_array(..., ..., sp_IntArray_get(...))
        # Cast third arg (index 2) to (sp_IntArray *)(intptr_t)
        line = cast_intarray_get_in_func_arg(line, "sp_set_node_field_array", 2, "sp_IntArray *")

        # Pattern: sp_SpNode_* functions with sp_IntArray_get as first arg
        %w[sp_SpNode_sp_type sp_SpNode_sp_type_is sp_SpNode_sp_has
           sp_SpNode_sp_child_nodes sp_SpNode__add_child
           sp_SpNode__add_children sp_SpNode_to_s].each do |fname|
          line = cast_intarray_get_in_func_arg(line, fname, 0, "sp_SpNode *")
        end

        line
      end
      fixed.join("\n")
    end

    # In +line+, find calls to +func_name+ and if the argument at +arg_index+
    # is a bare sp_IntArray_get(...) call, wrap it with (cast_type)(intptr_t).
    def cast_intarray_get_in_func_arg(line, func_name, arg_index, cast_type)
      result = ""
      remaining = line
      while (idx = remaining.index(func_name + "("))
        result << remaining[0...idx]
        remaining = remaining[idx..]
        open_idx = func_name.length  # index of '('

        # Find balanced closing paren of the outer function call
        depth = 1
        pos = open_idx + 1
        while pos < remaining.length && depth > 0
          depth += 1 if remaining[pos] == "("
          depth -= 1 if remaining[pos] == ")"
          pos += 1
        end
        # pos is right after closing paren
        call_text = remaining[0...pos]
        after = remaining[pos..]

        # Parse arguments using balanced paren tracking
        args = split_balanced_args(remaining[(open_idx + 1)...(pos - 1)])

        if args[arg_index] && args[arg_index].strip =~ /\Asp_IntArray_get\(/
          arg = args[arg_index].strip
          # Only cast if not already cast
          unless arg =~ /\A\(\s*#{Regexp.escape(cast_type)}\s*\)/
            args[arg_index] = " (#{cast_type})(intptr_t)#{arg}"
          end
          call_text = "#{func_name}(#{args.join(',')})"
        end

        result << call_text
        remaining = after
      end
      result << remaining
      result
    end

    # Split a string of function arguments by top-level commas (respecting parens).
    def split_balanced_args(text)
      args = []
      depth = 0
      current = ""
      text.each_char do |ch|
        if ch == "(" then depth += 1
        elsif ch == ")" then depth -= 1
        end
        if ch == "," && depth == 0
          args << current
          current = ""
        else
          current << ch
        end
      end
      args << current unless current.empty?
      args
    end

    def emit_string_helpers(out, before_str_array: true)
      # str_split, str_chars, str_slice_range depend on other types, emit them after
      unless before_str_array
        if @string_helpers_needed.include?(:str_slice_range)
          out.puts <<~C
            static const char *sp_str_slice_range(const char *s, sp_Range r) {
              size_t len = strlen(s);
              mrb_int start = r.first < 0 ? (mrb_int)len + r.first : r.first;
              mrb_int end_ = r.last < 0 ? (mrb_int)len + r.last : r.last;
              if (start < 0) start = 0;
              if (end_ >= (mrb_int)len) end_ = (mrb_int)len - 1;
              if (start > end_) { char *r2 = (char *)malloc(1); r2[0] = '\\0'; return r2; }
              size_t n = end_ - start + 1;
              char *r2 = (char *)malloc(n + 1);
              memcpy(r2, s + start, n); r2[n] = '\\0'; return r2;
            }
          C
        end

        if @string_helpers_needed.include?(:str_slice)
          out.puts <<~C
            static const char *sp_str_slice(const char *s, mrb_int start, mrb_int len) {
              size_t slen = strlen(s);
              if (start < 0) start = (mrb_int)slen + start;
              if (start < 0 || start >= (mrb_int)slen) { char *r = (char *)malloc(1); r[0] = '\\0'; return r; }
              if (start + len > (mrb_int)slen) len = (mrb_int)slen - start;
              char *r = (char *)malloc(len + 1);
              memcpy(r, s + start, len); r[len] = '\\0'; return r;
            }
          C
        end

        # Only emit StrArray-dependent helpers
        if @string_helpers_needed.include?(:str_split)
          out.puts <<~C
            static sp_StrArray *sp_str_split(const char *s, const char *delim) {
              sp_StrArray *a = sp_StrArray_new();
              size_t dl = strlen(delim);
              while (*s) {
                const char *p = strstr(s, delim);
                if (!p) { size_t n = strlen(s); char *t = (char *)malloc(n+1); memcpy(t,s,n+1); sp_StrArray_push(a, t); break; }
                size_t n = p - s; char *t = (char *)malloc(n+1); memcpy(t,s,n); t[n]='\\0'; sp_StrArray_push(a, t); s = p + dl;
              }
              return a;
            }
          C
        end
        if @string_helpers_needed.include?(:str_chars)
          out.puts <<~C
            static sp_StrArray *sp_str_chars(const char *s) {
              sp_StrArray *a = sp_StrArray_new();
              for (size_t i = 0; s[i]; i++) {
                char *c = (char *)malloc(2); c[0] = s[i]; c[1] = '\\0';
                sp_StrArray_push(a, c);
              } return a;
            }
          C
        end
        return
      end

      # Emit non-StrArray-dependent helpers
      if @string_helpers_needed.include?(:str_upcase)
        out.puts <<~C
          static const char *sp_str_upcase(const char *s) {
            size_t n = strlen(s); char *r = (char *)malloc(n + 1);
            for (size_t i = 0; i <= n; i++) r[i] = toupper((unsigned char)s[i]);
            return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_downcase)
        out.puts <<~C
          static const char *sp_str_downcase(const char *s) {
            size_t n = strlen(s); char *r = (char *)malloc(n + 1);
            for (size_t i = 0; i <= n; i++) r[i] = tolower((unsigned char)s[i]);
            return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_strip)
        out.puts <<~C
          static const char *sp_str_strip(const char *s) {
            while (*s && isspace((unsigned char)*s)) s++;
            size_t n = strlen(s);
            while (n > 0 && isspace((unsigned char)s[n-1])) n--;
            char *r = (char *)malloc(n + 1);
            memcpy(r, s, n); r[n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_tr)
        out.puts <<~C
          static const char *sp_str_tr(const char *s, const char *from, const char *to) {
            size_t n = strlen(s), fl = strlen(from), tl = strlen(to);
            char *r = (char *)malloc(n + 1);
            for (size_t i = 0; i < n; i++) {
              const char *p = memchr(from, s[i], fl);
              if (p) {
                size_t idx = p - from;
                r[i] = idx < tl ? to[idx] : to[tl - 1];
              } else {
                r[i] = s[i];
              }
            }
            r[n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_delete)
        out.puts <<~C
          static const char *sp_str_delete_chars(const char *s, const char *chars) {
            size_t n = strlen(s); char *r = (char *)malloc(n + 1); size_t ri = 0;
            for (size_t i = 0; i < n; i++) {
              if (!memchr(chars, s[i], strlen(chars))) r[ri++] = s[i];
            }
            r[ri] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_squeeze)
        out.puts <<~C
          static const char *sp_str_squeeze(const char *s) {
            size_t n = strlen(s); char *r = (char *)malloc(n + 1); size_t ri = 0;
            for (size_t i = 0; i < n; i++) {
              if (i == 0 || s[i] != s[i-1]) r[ri++] = s[i];
            }
            r[ri] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_ljust)
        out.puts <<~C
          static const char *sp_str_ljust(const char *s, mrb_int w, const char *pad) {
            size_t sl = strlen(s);
            if ((mrb_int)sl >= w) { char *r = (char *)malloc(sl + 1); strcpy(r, s); return r; }
            char *r = (char *)malloc(w + 1);
            memcpy(r, s, sl);
            for (size_t i = sl; i < (size_t)w; i++) r[i] = pad[0];
            r[w] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_rjust)
        out.puts <<~C
          static const char *sp_str_rjust(const char *s, mrb_int w, const char *pad) {
            size_t sl = strlen(s);
            if ((mrb_int)sl >= w) { char *r = (char *)malloc(sl + 1); strcpy(r, s); return r; }
            char *r = (char *)malloc(w + 1);
            size_t pad_len = w - sl;
            for (size_t i = 0; i < pad_len; i++) r[i] = pad[0];
            memcpy(r + pad_len, s, sl + 1);
            return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_center)
        out.puts <<~C
          static const char *sp_str_center(const char *s, mrb_int w) {
            size_t sl = strlen(s);
            if ((mrb_int)sl >= w) { char *r = (char *)malloc(sl + 1); strcpy(r, s); return r; }
            char *r = (char *)malloc(w + 1);
            size_t lpad = (w - sl) / 2;
            size_t rpad = w - sl - lpad;
            memset(r, ' ', lpad);
            memcpy(r + lpad, s, sl);
            memset(r + lpad + sl, ' ', rpad);
            r[w] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_lstrip)
        out.puts <<~C
          static const char *sp_str_lstrip(const char *s) {
            while (*s && isspace((unsigned char)*s)) s++;
            char *r = (char *)malloc(strlen(s) + 1);
            strcpy(r, s); return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_rstrip)
        out.puts <<~C
          static const char *sp_str_rstrip(const char *s) {
            size_t n = strlen(s);
            while (n > 0 && isspace((unsigned char)s[n-1])) n--;
            char *r = (char *)malloc(n + 1);
            memcpy(r, s, n); r[n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_chomp)
        out.puts <<~C
          static const char *sp_str_chomp(const char *s) {
            size_t n = strlen(s);
            if (n > 0 && s[n-1] == '\\n') { if (n > 1 && s[n-2] == '\\r') n--; n--; }
            char *r = (char *)malloc(n + 1);
            memcpy(r, s, n); r[n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_chop)
        out.puts <<~C
          static const char *sp_str_chop(const char *s) {
            size_t n = strlen(s);
            if (n > 0) n--;
            char *r = (char *)malloc(n + 1);
            memcpy(r, s, n); r[n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_capitalize)
        out.puts <<~C
          static const char *sp_str_capitalize(const char *s) {
            size_t n = strlen(s); char *r = (char *)malloc(n + 1);
            for (size_t i = 0; i <= n; i++) r[i] = (i == 0) ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]);
            return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_reverse)
        out.puts <<~C
          static const char *sp_str_reverse(const char *s) {
            size_t n = strlen(s); char *r = (char *)malloc(n + 1);
            for (size_t i = 0; i < n; i++) r[i] = s[n - 1 - i];
            r[n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_count)
        out.puts <<~C
          static mrb_int sp_str_count(const char *s, const char *chars) {
            mrb_int c = 0;
            for (; *s; s++) { for (const char *p = chars; *p; p++) { if (*s == *p) { c++; break; } } }
            return c;
          }
        C
      end

      if @string_helpers_needed.include?(:str_starts_with)
        out.puts <<~C
          static mrb_bool sp_str_starts_with(const char *s, const char *prefix) {
            size_t pn = strlen(prefix);
            return strncmp(s, prefix, pn) == 0;
          }
        C
      end

      if @string_helpers_needed.include?(:str_ends_with)
        out.puts <<~C
          static mrb_bool sp_str_ends_with(const char *s, const char *suffix) {
            size_t sn = strlen(s), xn = strlen(suffix);
            return sn >= xn && strcmp(s + sn - xn, suffix) == 0;
          }
        C
      end

      if @string_helpers_needed.include?(:str_gsub)
        out.puts <<~C
          static const char *sp_str_gsub(const char *s, const char *from, const char *to) {
            size_t fl = strlen(from), tl = strlen(to);
            size_t cap = strlen(s) * 2 + 16; char *r = (char *)malloc(cap); size_t ri = 0;
            while (*s) {
              if (strncmp(s, from, fl) == 0) {
                if (ri + tl >= cap) { cap = (ri + tl) * 2; r = (char *)realloc(r, cap); }
                memcpy(r + ri, to, tl); ri += tl; s += fl;
              }
              else {
                if (ri + 1 >= cap) { cap *= 2; r = (char *)realloc(r, cap); }
                r[ri++] = *s++;
              }
            }
            r[ri] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_sub)
        out.puts <<~C
          static const char *sp_str_sub(const char *s, const char *from, const char *to) {
            const char *p = strstr(s, from);
            if (!p) { size_t n = strlen(s); char *r = (char *)malloc(n+1); memcpy(r,s,n+1); return r; }
            size_t fl = strlen(from), tl = strlen(to), sn = strlen(s);
            char *r = (char *)malloc(sn - fl + tl + 1);
            size_t before = p - s;
            memcpy(r, s, before); memcpy(r + before, to, tl);
            memcpy(r + before + tl, p + fl, sn - before - fl + 1); return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_char_at)
        out.puts <<~C
          static const char *sp_str_char_at(const char *s, mrb_int idx) {
            mrb_int len = (mrb_int)strlen(s);
            if (idx < 0) idx += len;
            if (idx < 0 || idx >= len) return "";
            char *r = (char *)malloc(2); r[0] = s[idx]; r[1] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_concat)
        out.puts <<~C
          static const char *sp_str_concat(const char *a, const char *b) {
            size_t la = strlen(a), lb = strlen(b);
            char *r = (char *)malloc(la + lb + 1);
            memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r;
          }
        C
      end

      if @string_helpers_needed.include?(:int_to_s)
        out.puts <<~C
          static const char *sp_int_to_s(mrb_int n) {
            char *r = (char *)malloc(24); snprintf(r, 24, "%lld", (long long)n); return r;
          }
        C
      end

      if @string_helpers_needed.include?(:str_repeat)
        out.puts <<~C
          static const char *sp_str_repeat(const char *s, mrb_int n) {
            size_t sl = strlen(s); char *r = (char *)malloc(sl * n + 1);
            for (mrb_int i = 0; i < n; i++) memcpy(r + sl * i, s, sl);
            r[sl * n] = '\\0'; return r;
          }
        C
      end

      if @string_helpers_needed.include?(:float_to_s)
        out.puts <<~C
          static const char *sp_float_to_s(mrb_float f) {
            char *r = (char *)malloc(32);
            snprintf(r, 32, "%g", f);
            if (!strchr(r, '.') && !strchr(r, 'e') && !strchr(r, 'E')) strcat(r, ".0");
            return r;
          }
        C
      end

    end

    def emit_time_runtime(out)
      out.puts <<~C

        /* ---- Built-in time ---- */
        #include <time.h>
        typedef struct { time_t t; } sp_Time;
        static sp_Time sp_Time_now(void) { sp_Time r; r.t = time(NULL); return r; }
        static sp_Time sp_Time_at(mrb_int n) { sp_Time r; r.t = (time_t)n; return r; }
        static mrb_int sp_Time_to_i(sp_Time t) { return (mrb_int)t.t; }
        static mrb_int sp_Time_diff(sp_Time a, sp_Time b) { return (mrb_int)(a.t - b.t); }

      C
    end

    def emit_file_runtime(out)
      out.puts <<~C

        /* ---- Built-in file I/O ---- */
        typedef struct { FILE *fp; } sp_File;

        static sp_File *sp_File_open(const char *path, const char *mode) {
          sp_File *f = (sp_File *)calloc(1, sizeof(sp_File));
          f->fp = fopen(path, mode);
          return f;
        }

        static void sp_File_close(sp_File *f) {
          if (f && f->fp) { fclose(f->fp); f->fp = NULL; }
        }

        static void sp_File_puts(sp_File *f, const char *s) {
          if (f && f->fp) { fputs(s, f->fp); fputc('\\n', f->fp); }
        }

        static void sp_File_print(sp_File *f, const char *s) {
          if (f && f->fp) { fputs(s, f->fp); }
        }

        static const char *sp_File_read(const char *path) {
          FILE *f = fopen(path, "r");
          if (!f) return "";
          fseek(f, 0, SEEK_END);
          long len = ftell(f);
          fseek(f, 0, SEEK_SET);
          char *buf = (char *)malloc(len + 1);
          if (fread(buf, 1, len, f)) {}
          buf[len] = '\\0';
          fclose(f);
          return buf;
        }

        static mrb_int sp_File_write(const char *path, const char *data) {
          FILE *f = fopen(path, "w");
          if (!f) return 0;
          mrb_int n = (mrb_int)fwrite(data, 1, strlen(data), f);
          fclose(f);
          return n;
        }

        static mrb_bool sp_File_exist(const char *path) {
          struct stat st;
          return stat(path, &st) == 0;
        }

        static mrb_int sp_File_delete(const char *path) {
          return remove(path) == 0 ? 1 : 0;
        }

        static const char *sp_File_join(const char *a, const char *b) {
          size_t la = strlen(a), lb = strlen(b);
          char *r = (char *)malloc(la + lb + 2);
          memcpy(r, a, la);
          r[la] = '/';
          memcpy(r + la + 1, b, lb + 1);
          return r;
        }

      C
    end

    def box_value(val, orig_type)
      case orig_type
      when Type::INTEGER then "sp_box_int(#{val})"
      when Type::FLOAT then "sp_box_float(#{val})"
      when Type::STRING, Type::SYMBOL then "sp_box_str(#{val})"
      when Type::BOOLEAN then "sp_box_bool(#{val})"
      when Type::NIL then "sp_box_nil()"
      when Type::POLY then val  # already boxed
      else
        # Check if it's a class type
        if orig_type.is_a?(String) && @class_tags && @class_tags[orig_type]
          "sp_box_obj(SP_TAG_#{orig_type}, #{val})"
        else
          "sp_box_int(#{val})"
        end
      end
    end

    def emit_poly_runtime(out)
      out.puts <<~C

        /* NaN-boxing: 8-byte tagged value */
        typedef uint64_t sp_RbValue;
        #define SP_T_OBJECT 0x0000
        #define SP_T_INT    0x0001
        #define SP_T_STRING 0x0002
        #define SP_T_BOOL   0x0003
        #define SP_T_NIL    0x0004
        #define SP_T_FLOAT  0x0005
        #define SP_TAG(v)       ((uint16_t)((v) >> 48))
        #define SP_PAYLOAD(v)   ((v) & 0x0000FFFFFFFFFFFFULL)
        #define SP_IS_INT(v)    (SP_TAG(v) == SP_T_INT)
        #define SP_IS_STR(v)    (SP_TAG(v) == SP_T_STRING)
        #define SP_IS_BOOL(v)   (SP_TAG(v) == SP_T_BOOL)
        #define SP_IS_NIL(v)    (SP_TAG(v) == SP_T_NIL)
        #define SP_IS_DBL(v)    (SP_TAG(v) == SP_T_FLOAT)
        #define SP_IS_OBJ(v)    (SP_TAG(v) >= 0x0040)
      C
      # Emit class tag defines
      if @class_tags && !@class_tags.empty?
        @class_tags.each do |cname, tag|
          out.puts "#define SP_TAG_#{cname} 0x#{tag.to_s(16).rjust(4, '0')}"
        end
      end
      out.puts <<~C

        static sp_RbValue sp_box_int(int64_t n) { return ((uint64_t)0x0001 << 48) | ((uint64_t)n & 0x0000FFFFFFFFFFFFULL); }
        static sp_RbValue sp_box_float(double f) { uint64_t b; memcpy(&b, &f, 8); return ((uint64_t)0x0005 << 48) | (b >> 16); }
        static sp_RbValue sp_box_str(const char *s) { return ((uint64_t)0x0002 << 48) | (uint64_t)(uintptr_t)s; }
        static sp_RbValue sp_box_bool(int b) { return ((uint64_t)0x0003 << 48) | (b ? 1 : 0); }
        static sp_RbValue sp_box_nil(void) { return ((uint64_t)0x0004 << 48); }
        static sp_RbValue sp_box_obj(int tag, void *p) { return ((uint64_t)(unsigned)tag << 48) | (uint64_t)(uintptr_t)p; }

        static int64_t sp_unbox_int(sp_RbValue v) { int64_t raw = (int64_t)(v & 0x0000FFFFFFFFFFFFULL); return (raw << 16) >> 16; }
        static double sp_unbox_float(sp_RbValue v) { uint64_t b = (v & 0x0000FFFFFFFFFFFFULL) << 16; double f; memcpy(&f, &b, 8); return f; }
        static const char *sp_unbox_str(sp_RbValue v) { return (const char *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }
        static void *sp_unbox_obj(sp_RbValue v) { return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }
        static int64_t sp_unbox_bool(sp_RbValue v) { return (int64_t)(v & 1); }

        static void sp_poly_puts(sp_RbValue v) {
          uint16_t t = SP_TAG(v);
          if (t == SP_T_INT) { printf("%lld\\n", (long long)sp_unbox_int(v)); }
          else if (t == SP_T_FLOAT) { char buf[32]; snprintf(buf,32,"%g",sp_unbox_float(v));
            printf("%s%s\\n", buf, strchr(buf,'.')||strchr(buf,'e')?"":".0"); }
          else if (t == SP_T_STRING) { const char *s=sp_unbox_str(v); fputs(s,stdout);
            if(!*s||s[strlen(s)-1]!='\\n') putchar('\\n'); }
          else if (t == SP_T_BOOL) { puts(sp_unbox_bool(v) ? "true" : "false"); }
          else if (t == SP_T_NIL) { puts(""); }
          else { puts("(object)"); }
        }
        static mrb_bool sp_poly_nil_p(sp_RbValue v) { return SP_TAG(v) == SP_T_NIL; }

        static sp_RbValue sp_poly_add(sp_RbValue a, sp_RbValue b) {
          uint16_t ta = SP_TAG(a), tb = SP_TAG(b);
          if (ta == SP_T_INT && tb == SP_T_INT) return sp_box_int(sp_unbox_int(a) + sp_unbox_int(b));
          if (ta == SP_T_FLOAT || tb == SP_T_FLOAT) {
            double fa = (ta == SP_T_FLOAT) ? sp_unbox_float(a) : (double)sp_unbox_int(a);
            double fb = (tb == SP_T_FLOAT) ? sp_unbox_float(b) : (double)sp_unbox_int(b);
            return sp_box_float(fa + fb);
          }
          if (ta == SP_T_STRING && tb == SP_T_STRING) {
            const char *sa = sp_unbox_str(a), *sb = sp_unbox_str(b);
            size_t la = strlen(sa), lb = strlen(sb);
            char *r = (char *)malloc(la + lb + 1);
            memcpy(r, sa, la); memcpy(r + la, sb, lb + 1);
            return sp_box_str(r);
          }
          return sp_box_int(0);
        }
        static sp_RbValue sp_poly_sub(sp_RbValue a, sp_RbValue b) {
          uint16_t ta = SP_TAG(a), tb = SP_TAG(b);
          if (ta == SP_T_FLOAT || tb == SP_T_FLOAT) {
            double fa = (ta == SP_T_FLOAT) ? sp_unbox_float(a) : (double)sp_unbox_int(a);
            double fb = (tb == SP_T_FLOAT) ? sp_unbox_float(b) : (double)sp_unbox_int(b);
            return sp_box_float(fa - fb);
          }
          return sp_box_int(sp_unbox_int(a) - sp_unbox_int(b));
        }
        static sp_RbValue sp_poly_mul(sp_RbValue a, sp_RbValue b) {
          uint16_t ta = SP_TAG(a), tb = SP_TAG(b);
          if (ta == SP_T_FLOAT || tb == SP_T_FLOAT) {
            double fa = (ta == SP_T_FLOAT) ? sp_unbox_float(a) : (double)sp_unbox_int(a);
            double fb = (tb == SP_T_FLOAT) ? sp_unbox_float(b) : (double)sp_unbox_int(b);
            return sp_box_float(fa * fb);
          }
          return sp_box_int(sp_unbox_int(a) * sp_unbox_int(b));
        }
        static sp_RbValue sp_poly_div(sp_RbValue a, sp_RbValue b) {
          uint16_t ta = SP_TAG(a), tb = SP_TAG(b);
          if (ta == SP_T_FLOAT || tb == SP_T_FLOAT) {
            double fa = (ta == SP_T_FLOAT) ? sp_unbox_float(a) : (double)sp_unbox_int(a);
            double fb = (tb == SP_T_FLOAT) ? sp_unbox_float(b) : (double)sp_unbox_int(b);
            return sp_box_float(fa / fb);
          }
          return sp_box_int(sp_unbox_int(b) != 0 ? sp_unbox_int(a) / sp_unbox_int(b) : 0);
        }
        static sp_RbValue sp_poly_mod(sp_RbValue a, sp_RbValue b) {
          return sp_box_int(sp_unbox_int(b) != 0 ? sp_unbox_int(a) % sp_unbox_int(b) : 0);
        }
        static mrb_bool sp_poly_gt(sp_RbValue a, sp_RbValue b) {
          uint16_t ta = SP_TAG(a), tb = SP_TAG(b);
          if (ta == SP_T_FLOAT || tb == SP_T_FLOAT) {
            double fa = (ta == SP_T_FLOAT) ? sp_unbox_float(a) : (double)sp_unbox_int(a);
            double fb = (tb == SP_T_FLOAT) ? sp_unbox_float(b) : (double)sp_unbox_int(b);
            return fa > fb;
          }
          return sp_unbox_int(a) > sp_unbox_int(b);
        }
        static mrb_bool sp_poly_lt(sp_RbValue a, sp_RbValue b) {
          uint16_t ta = SP_TAG(a), tb = SP_TAG(b);
          if (ta == SP_T_FLOAT || tb == SP_T_FLOAT) {
            double fa = (ta == SP_T_FLOAT) ? sp_unbox_float(a) : (double)sp_unbox_int(a);
            double fb = (tb == SP_T_FLOAT) ? sp_unbox_float(b) : (double)sp_unbox_int(b);
            return fa < fb;
          }
          return sp_unbox_int(a) < sp_unbox_int(b);
        }
        static mrb_bool sp_poly_ge(sp_RbValue a, sp_RbValue b) { return !sp_poly_lt(a, b); }
        static mrb_bool sp_poly_le(sp_RbValue a, sp_RbValue b) { return !sp_poly_gt(a, b); }

        static const char *sp_poly_to_s(sp_RbValue v) {
          uint16_t t = SP_TAG(v);
          if (t == SP_T_INT) { char *r=(char*)malloc(24); snprintf(r,24,"%lld",(long long)sp_unbox_int(v)); return r; }
          if (t == SP_T_FLOAT) { char *r=(char*)malloc(32); snprintf(r,32,"%g",sp_unbox_float(v));
            if (!strchr(r,'.') && !strchr(r,'e') && !strchr(r,'E')) strcat(r,".0"); return r; }
          if (t == SP_T_STRING) return sp_unbox_str(v);
          if (t == SP_T_BOOL) return sp_unbox_bool(v) ? "true" : "false";
          if (t == SP_T_NIL) return "";
          return "(object)";
        }

      C
    end

    def emit_stringio_runtime(out)
      out.puts <<~C

        /* ---- StringIO runtime ---- */
        typedef struct {
          char *buf; int64_t len; int64_t cap; int64_t pos; int64_t lineno; int closed;
        } sp_StringIO;
        static void sio_grow(sp_StringIO *sio, int64_t need) {
          int64_t req = sio->pos + need; if (req <= sio->cap) return;
          int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2;
          sio->buf = (char *)realloc(sio->buf, nc + 1); sio->cap = nc;
        }
        static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) {
          sio_grow(sio, dl);
          if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len);
          memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl;
          if (sio->pos > sio->len) sio->len = sio->pos;
          sio->buf[sio->len] = '\\0'; return dl;
        }
        static sp_StringIO *sp_StringIO_new(void) {
          sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO));
          s->buf = (char *)calloc(1, 64); s->cap = 63; return s;
        }
        static sp_StringIO *sp_StringIO_new_s(const char *init) {
          sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO));
          int64_t l = (int64_t)strlen(init); int64_t c = l < 63 ? 63 : l;
          s->buf = (char *)malloc(c+1); memcpy(s->buf, init, l); s->buf[l]='\\0';
          s->len = l; s->cap = c; return s;
        }
        static const char *sp_StringIO_string(sp_StringIO *s) { return s->buf ? s->buf : ""; }
        static int64_t sp_StringIO_pos(sp_StringIO *s) { return s->pos; }
        static int64_t sp_StringIO_lineno(sp_StringIO *s) { return s->lineno; }
        static int64_t sp_StringIO_size(sp_StringIO *s) { return s->len; }
        static int64_t sp_StringIO_write(sp_StringIO *s, const char *str) {
          return sio_write(s, str, (int64_t)strlen(str));
        }
        static int64_t sp_StringIO_puts(sp_StringIO *s, const char *str) {
          int64_t l = (int64_t)strlen(str); sio_write(s, str, l);
          if (l == 0 || str[l-1] != '\\n') sio_write(s, "\\n", 1); return 0;
        }
        static int64_t sp_StringIO_puts_empty(sp_StringIO *s) { sio_write(s, "\\n", 1); return 0; }
        static int64_t sp_StringIO_print(sp_StringIO *s, const char *str) {
          return sio_write(s, str, (int64_t)strlen(str));
        }
        static int64_t sp_StringIO_putc(sp_StringIO *s, int64_t ch) {
          char c = (char)(ch & 0xFF); sio_write(s, &c, 1); return ch;
        }
        static sp_StringIO *sp_StringIO_append(sp_StringIO *s, const char *str) {
          sio_write(s, str, (int64_t)strlen(str)); return s;
        }
        static const char *sp_StringIO_read(sp_StringIO *s) {
          if (s->pos >= s->len) return "";
          const char *r = s->buf + s->pos; s->pos = s->len; return r;
        }
        static const char *sp_StringIO_read_n(sp_StringIO *s, int64_t n) {
          if (s->pos >= s->len) return NULL;
          int64_t rem = s->len - s->pos; if (n > rem) n = rem;
          static char rb[65536]; if (n >= (int64_t)sizeof(rb)) n = sizeof(rb)-1;
          memcpy(rb, s->buf + s->pos, n); rb[n] = '\\0'; s->pos += n; return rb;
        }
        static const char *sp_StringIO_gets(sp_StringIO *s) {
          if (s->pos >= s->len) return NULL;
          const char *st = s->buf + s->pos; const char *nl = memchr(st, '\\n', s->len - s->pos);
          int64_t ll = nl ? (nl - st) + 1 : s->len - s->pos;
          static char gb[65536]; if (ll >= (int64_t)sizeof(gb)) ll = sizeof(gb)-1;
          memcpy(gb, st, ll); gb[ll] = '\\0'; s->pos += ll; s->lineno++; return gb;
        }
        static const char *sp_StringIO_getc(sp_StringIO *s) {
          if (s->pos >= s->len) return NULL;
          static char gc[2]; gc[0] = s->buf[s->pos++]; gc[1] = '\\0'; return gc;
        }
        static int64_t sp_StringIO_getbyte(sp_StringIO *s) {
          if (s->pos >= s->len) return -1;
          return (int64_t)(unsigned char)s->buf[s->pos++];
        }
        static int64_t sp_StringIO_rewind(sp_StringIO *s) { s->pos = 0; s->lineno = 0; return 0; }
        static int64_t sp_StringIO_seek(sp_StringIO *s, int64_t off, int64_t w) {
          int64_t np; switch(w) { case 1: np=s->pos+off; break; case 2: np=s->len+off; break; default: np=off; }
          if (np < 0) np = 0; s->pos = np; return 0;
        }
        static mrb_bool sp_StringIO_eof_p(sp_StringIO *s) { return s->pos >= s->len; }
        static int64_t sp_StringIO_truncate(sp_StringIO *s, int64_t l) {
          if (l < 0) l = 0; if (l < s->len) { s->len = l; s->buf[l] = '\\0'; } return 0;
        }
        static int64_t sp_StringIO_close(sp_StringIO *s) { s->closed = 1; return 0; }
        static mrb_bool sp_StringIO_closed_p(sp_StringIO *s) { return s->closed; }
        static sp_StringIO *sp_StringIO_flush(sp_StringIO *s) { return s; }
        static mrb_bool sp_StringIO_sync(sp_StringIO *s) { (void)s; return 1; }
        static mrb_bool sp_StringIO_isatty(sp_StringIO *s) { (void)s; return 0; }
        static int64_t sp_StringIO_fileno(sp_StringIO *s) { (void)s; return -1; }

      C
    end

    def emit_system_runtime(out)
      out.puts <<~C

        /* ---- System/exec runtime ---- */
        static mrb_bool sp_system(const char *cmd) {
          fflush(stdout);
          int r = system(cmd);
          sp_last_status = r;
          return r == 0;
        }

        static const char *sp_backtick(const char *cmd) {
          FILE *fp = popen(cmd, "r");
          if (!fp) return "";
          char buf[4096];
          size_t total = 0;
          char *result = (char *)malloc(4096);
          result[0] = '\\0';
          while (fgets(buf, sizeof(buf), fp)) {
            size_t bl = strlen(buf);
            result = (char *)realloc(result, total + bl + 1);
            memcpy(result + total, buf, bl + 1);
            total += bl;
          }
          sp_last_status = pclose(fp);
          return result;
        }

      C
    end

    def generate_exc_hierarchy_check
      # Generate class hierarchy checks for exception classes
      lines = []
      @classes.each do |cname, ci|
        # For each exception class, check if the raised class is a subclass
        parent = ci.parent
        ancestors = [cname]
        while parent
          ancestors << parent
          pci = @classes[parent]
          parent = pci ? pci.parent : nil
        end
        # If this class has parents, generate: if raised is cname and checking for parent, match
        ancestors[1..].each do |ancestor|
          lines << "if (strcmp(sp_exc_class, \"#{cname}\") == 0 && strcmp(cls, \"#{ancestor}\") == 0) return 1;"
        end
      end
      # Match RuntimeError as base only if no custom classes defined
      if lines.empty?
        lines << 'if (strcmp(cls, "RuntimeError") == 0) return 1;'
      end
      lines.join("\n          ")
    end

    def emit_regexp_runtime(out)
      out.puts <<~C
        /* ---- Regexp runtime (oniguruma) ---- */
        /* Link with: /usr/lib/x86_64-linux-gnu/libonig.so.5 */
        /* Minimal oniguruma declarations - no header needed */
        typedef unsigned char OnigUChar;
        typedef struct OnigEncodingTypeST OnigEncodingType;
        typedef OnigEncodingType *OnigEncoding;
        typedef struct { int num_regs; int *beg; int *end; } OnigRegion;
        typedef struct re_pattern_buffer regex_t;
        typedef struct { int ret; const OnigUChar *s; } OnigErrorInfo;
        typedef struct OnigSyntaxTypeST OnigSyntaxType;
        #define ONIG_OPTION_DEFAULT 0
        #define ONIG_OPTION_NONE 0
        extern OnigEncodingType OnigEncodingUTF8;
        extern OnigSyntaxType OnigSyntaxRuby;
        extern int onig_initialize(OnigEncoding *encs, int n);
        extern int onig_new(regex_t **reg, const OnigUChar *pattern,
          const OnigUChar *pattern_end, int option, OnigEncoding enc,
          OnigSyntaxType *syntax, OnigErrorInfo *einfo);
        extern int onig_search(regex_t *reg, const OnigUChar *str,
          const OnigUChar *end, const OnigUChar *start, const OnigUChar *range,
          OnigRegion *region, int option);
        extern OnigRegion *onig_region_new(void);
        extern void onig_region_free(OnigRegion *region, int free_self);

        static OnigRegion *sp_match_region;
        static const char *sp_match_str;
      C

      # Declare regex variables
      @regexp_patterns.each do |rp|
        out.puts "static regex_t *#{rp[:c_var]};"
      end
      out.puts

      # sp_regexp_init function
      out.puts "static void sp_regexp_init(void) {"
      out.puts "  OnigEncoding enc = &OnigEncodingUTF8;"
      out.puts "  OnigErrorInfo einfo;"
      out.puts "  onig_initialize(&enc, 1);"
      out.puts "  sp_match_region = onig_region_new();"
      @regexp_patterns.each do |rp|
        pat = rp[:pattern]
        # C-escape the pattern: backslashes need doubling
        c_pat = pat.gsub('\\', '\\\\\\\\').gsub('"', '\\"')
        pat_len = pat.length
        out.puts "  onig_new(&#{rp[:c_var]}, (const OnigUChar *)\"#{c_pat}\", (const OnigUChar *)\"#{c_pat}\" + #{pat_len},"
        out.puts "    ONIG_OPTION_DEFAULT, &OnigEncodingUTF8, &OnigSyntaxRuby, &einfo);"
      end
      out.puts "}"
      out.puts

      # Helper functions
      out.puts <<~C
        static mrb_int sp_re_match(regex_t *re, const char *s) {
          sp_match_str = s;
          const OnigUChar *end = (const OnigUChar *)s + strlen(s);
          int r = onig_search(re, (const OnigUChar *)s, end,
            (const OnigUChar *)s, end, sp_match_region, ONIG_OPTION_NONE);
          return (mrb_int)r;
        }

        static mrb_bool sp_re_match_p(regex_t *re, const char *s) {
          const OnigUChar *end = (const OnigUChar *)s + strlen(s);
          int r = onig_search(re, (const OnigUChar *)s, end,
            (const OnigUChar *)s, end, sp_match_region, ONIG_OPTION_NONE);
          return r >= 0;
        }

        static const char *sp_re_group(int n) {
          if (n < 0 || n >= sp_match_region->num_regs) return "";
          int beg = sp_match_region->beg[n], end = sp_match_region->end[n];
          if (beg < 0) return "";
          int len = end - beg;
          char *r = (char *)malloc(len + 1);
          memcpy(r, sp_match_str + beg, len);
          r[len] = '\\0';
          return r;
        }

        static const char *sp_re_gsub(regex_t *re, const char *s, const char *repl) {
          size_t slen = strlen(s), rlen = strlen(repl);
          size_t cap = slen * 2 + 16; char *out = (char *)malloc(cap); size_t oi = 0;
          OnigRegion *region = onig_region_new();
          const OnigUChar *end = (const OnigUChar *)s + slen;
          int pos = 0;
          while (pos <= (int)slen) {
            int r = onig_search(re, (const OnigUChar *)s, end,
              (const OnigUChar *)s + pos, end, region, ONIG_OPTION_NONE);
            if (r < 0) break;
            int mbeg = region->beg[0], mend = region->end[0];
            size_t need = oi + (mbeg - pos) + rlen + (slen - mend) + 1;
            if (need > cap) { cap = need * 2; out = (char *)realloc(out, cap); }
            memcpy(out + oi, s + pos, mbeg - pos); oi += mbeg - pos;
            memcpy(out + oi, repl, rlen); oi += rlen;
            pos = mend;
            if (mend == mbeg) pos++;
          }
          size_t rest = slen - pos;
          if (oi + rest + 1 > cap) { cap = oi + rest + 1; out = (char *)realloc(out, cap); }
          memcpy(out + oi, s + pos, rest); oi += rest;
          out[oi] = '\\0';
          onig_region_free(region, 1);
          return out;
        }

        static const char *sp_re_sub(regex_t *re, const char *s, const char *repl) {
          size_t slen = strlen(s), rlen = strlen(repl);
          OnigRegion *region = onig_region_new();
          const OnigUChar *end = (const OnigUChar *)s + slen;
          int r = onig_search(re, (const OnigUChar *)s, end,
            (const OnigUChar *)s, end, region, ONIG_OPTION_NONE);
          if (r < 0) {
            onig_region_free(region, 1);
            char *dup = (char *)malloc(slen + 1); memcpy(dup, s, slen + 1); return dup;
          }
          int mbeg = region->beg[0], mend = region->end[0];
          size_t olen = slen - (mend - mbeg) + rlen;
          char *out = (char *)malloc(olen + 1);
          memcpy(out, s, mbeg);
          memcpy(out + mbeg, repl, rlen);
          memcpy(out + mbeg + rlen, s + mend, slen - mend + 1);
          onig_region_free(region, 1);
          return out;
        }
      C

      # sp_re_split only if str_array is needed
      if @needs_str_array
        out.puts <<~C
          static sp_StrArray *sp_re_split(regex_t *re, const char *s) {
            sp_StrArray *a = sp_StrArray_new();
            size_t slen = strlen(s);
            OnigRegion *region = onig_region_new();
            const OnigUChar *end = (const OnigUChar *)s + slen;
            int pos = 0;
            while (pos <= (int)slen) {
              int r = onig_search(re, (const OnigUChar *)s, end,
                (const OnigUChar *)s + pos, end, region, ONIG_OPTION_NONE);
              if (r < 0) break;
              int mbeg = region->beg[0], mend = region->end[0];
              int plen = mbeg - pos;
              char *part = (char *)malloc(plen + 1);
              memcpy(part, s + pos, plen); part[plen] = '\\0';
              sp_StrArray_push(a, part);
              pos = mend;
              if (mend == mbeg) pos++;
            }
            if (pos <= (int)slen) {
              int rlen = (int)slen - pos;
              char *part = (char *)malloc(rlen + 1);
              memcpy(part, s + pos, rlen); part[rlen] = '\\0';
              sp_StrArray_push(a, part);
            }
            onig_region_free(region, 1);
            return a;
          }
        C
      end
    end

    def emit_exception_runtime(out)
      out.puts <<~C

        /* ---- Exception handling runtime (setjmp/longjmp) ---- */
        #include <setjmp.h>
        #define SP_EXC_STACK_SIZE 64
        static jmp_buf sp_exc_stack[SP_EXC_STACK_SIZE];
        static int sp_exc_depth = 0;
        static const char *sp_exc_message = NULL;
        static const char *sp_exc_class = "RuntimeError";

        static void sp_raise(const char *msg) {
          sp_exc_message = msg; sp_exc_class = "RuntimeError";
          if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 1);
          fprintf(stderr, "unhandled exception: %s\\n", msg); exit(1);
        }
        static void sp_raise_cls(const char *cls, const char *msg) {
          sp_exc_message = msg; sp_exc_class = cls;
          if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 1);
          fprintf(stderr, "%s: %s\\n", cls, msg); exit(1);
        }
        static int sp_exc_is_a(const char *cls) {
          if (strcmp(sp_exc_class, cls) == 0) return 1;
          if (strcmp(cls, "StandardError") == 0 || strcmp(cls, "Exception") == 0) return 1;
          /* Check class hierarchy */
          #{generate_exc_hierarchy_check}
          return 0;
        }

      C

      if @needs_catch_throw
        out.puts <<~C

          /* ---- Catch/throw runtime ---- */
          static const char *sp_throw_tag = NULL;
          static mrb_int sp_throw_value_i = 0;
          static const char *sp_throw_value_s = NULL;
          static int sp_throw_is_str = 0;
          static void sp_throw_i(const char *tag, mrb_int val) {
            sp_throw_tag = tag; sp_throw_value_i = val; sp_throw_is_str = 0;
            if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 2);
            fprintf(stderr, "uncaught throw :\\"%s\\"\\n", tag); exit(1);
          }
          static void sp_throw_s(const char *tag, const char *val) {
            sp_throw_tag = tag; sp_throw_value_s = val; sp_throw_is_str = 1;
            if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 2);
            fprintf(stderr, "uncaught throw :\\"%s\\"\\n", tag); exit(1);
          }

        C
      end
    end

    def emit_gc_runtime(out)
      out.puts <<~C

        /* ---- Mark-and-sweep GC runtime ---- */
        typedef struct sp_gc_hdr {
          struct sp_gc_hdr *next;
          void (*finalize)(void *);
          void (*scan)(void *);
          unsigned marked : 1;
        } sp_gc_hdr;

        static sp_gc_hdr *sp_gc_heap = NULL;
        static size_t sp_gc_bytes = 0;
        static size_t sp_gc_threshold = 256 * 1024;

        #define SP_GC_STACK_MAX 8192
        static void **sp_gc_roots[SP_GC_STACK_MAX];
        static int sp_gc_nroots = 0;
        #define SP_GC_SAVE() int _gc_saved = sp_gc_nroots
        #define SP_GC_ROOT(v) do { if (sp_gc_nroots < SP_GC_STACK_MAX) sp_gc_roots[sp_gc_nroots++] = (void **)&(v); } while(0)
        #define SP_GC_RESTORE() sp_gc_nroots = _gc_saved

        static void sp_gc_mark(void *obj) {
          if (!obj) return;
          sp_gc_hdr *h = (sp_gc_hdr *)((char *)obj - sizeof(sp_gc_hdr));
          if (h->marked) return;
          h->marked = 1;
          if (h->scan) h->scan(obj);
        }

        static void sp_gc_collect(void) {
          for (int i = 0; i < sp_gc_nroots; i++) {
            void *obj = *sp_gc_roots[i];
            if (obj) sp_gc_mark(obj);
          }
          sp_gc_hdr **pp = &sp_gc_heap;
          sp_gc_bytes = 0;
          while (*pp) {
            sp_gc_hdr *h = *pp;
            if (!h->marked) {
              *pp = h->next;
              if (h->finalize) h->finalize((char *)h + sizeof(sp_gc_hdr));
              free(h);
            }
            else {
              h->marked = 0;
              sp_gc_bytes += sizeof(sp_gc_hdr);
              pp = &h->next;
            }
          }
        }

        static void *sp_gc_alloc(size_t sz, void (*finalize)(void *), void (*scan)(void *)) {
          if (sp_gc_bytes > sp_gc_threshold) {
            sp_gc_collect();
            if (sp_gc_bytes > sp_gc_threshold / 2)
              sp_gc_threshold *= 2;
          }
          sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, sizeof(sp_gc_hdr) + sz);
          h->finalize = finalize;
          h->scan = scan;
          h->next = sp_gc_heap;
          sp_gc_heap = h;
          sp_gc_bytes += sizeof(sp_gc_hdr) + sz;
          return (char *)h + sizeof(sp_gc_hdr);
        }

      C
    end

    def emit_int_array(out)
      out.puts <<~C
        /* ---- Built-in integer array ---- */
        typedef struct { mrb_int *data; mrb_int start; mrb_int len; mrb_int cap; } sp_IntArray;

      C

      if @needs_range
        out.puts <<~C
          /* ---- Built-in integer range ---- */
          typedef struct { mrb_int first; mrb_int last; } sp_Range;
          static sp_Range sp_Range_new(mrb_int first, mrb_int last) {
            sp_Range r; r.first = first; r.last = last; return r;
          }
          static mrb_bool sp_Range_include_p(sp_Range r, mrb_int v) {
            return v >= r.first && v <= r.last;
          }
          static sp_IntArray *sp_IntArray_from_range(mrb_int, mrb_int);
          static sp_IntArray *sp_Range_to_a(sp_Range r) {
            return sp_IntArray_from_range(r.first, r.last);
          }

        C
      end

      out.puts <<~C
        static void sp_IntArray_finalize(void *p) {
          sp_IntArray *a = (sp_IntArray *)p;
          free(a->data);
        }

        static sp_IntArray *sp_IntArray_new(void) {
          sp_IntArray *a = (sp_IntArray *)sp_gc_alloc(sizeof(sp_IntArray), sp_IntArray_finalize, NULL);
          a->cap = 16; a->data = (mrb_int *)malloc(sizeof(mrb_int) * a->cap);
          sp_gc_bytes += sizeof(mrb_int) * a->cap;
          return a;
        }

        static sp_IntArray *sp_IntArray_from_range(mrb_int start, mrb_int end) {
          sp_IntArray *a = sp_IntArray_new();
          mrb_int n = end - start + 1; if (n < 0) n = 0;
          if (n > a->cap) { sp_gc_bytes += sizeof(mrb_int) * (n - a->cap); a->cap = n; a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap); }
          for (mrb_int i = 0; i < n; i++) a->data[i] = start + i;
          a->len = n; return a;
        }

        static sp_IntArray *sp_IntArray_dup(sp_IntArray *a) {
          sp_IntArray *b = sp_IntArray_new();
          if (a->len > b->cap) { sp_gc_bytes += sizeof(mrb_int) * (a->len - b->cap); b->cap = a->len; b->data = (mrb_int *)realloc(b->data, sizeof(mrb_int) * b->cap); }
          memcpy(b->data, a->data + a->start, sizeof(mrb_int) * a->len);
          b->len = a->len; return b;
        }

        static void sp_IntArray_push(sp_IntArray *a, mrb_int val) {
          mrb_int end = a->start + a->len;
          if (end >= a->cap) {
            if (a->start > 0) { memmove(a->data, a->data + a->start, sizeof(mrb_int) * a->len); a->start = 0; end = a->len; }
            if (end >= a->cap) { a->cap = a->cap * 2 + 1; a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap); }
          }
          a->data[end] = val; a->len++;
        }

        static mrb_int sp_IntArray_unshift(sp_IntArray *a, mrb_int val) {
          if (a->start > 0) { a->data[--a->start] = val; a->len++; return val; }
          mrb_int end = a->start + a->len;
          if (end >= a->cap) { a->cap = a->cap * 2 + 1; a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap); }
          for (mrb_int i = end; i > a->start; i--) a->data[i] = a->data[i-1];
          a->data[a->start] = val; a->len++;
          return val;
        }

        static mrb_int sp_IntArray_shift(sp_IntArray *a) {
          mrb_int v = a->data[a->start++]; a->len--; return v;
        }

        static mrb_int sp_IntArray_pop(sp_IntArray *a) {
          return a->data[a->start + --a->len];
        }

        static mrb_bool sp_IntArray_empty(sp_IntArray *a) {
          return a->len == 0;
        }

        static void sp_IntArray_reverse_bang(sp_IntArray *a) {
          for (mrb_int i = 0, j = a->len - 1; i < j; i++, j--) {
            mrb_int t = a->data[a->start+i]; a->data[a->start+i] = a->data[a->start+j]; a->data[a->start+j] = t;
          }
        }

        static int _sp_int_cmp(const void *a, const void *b) {
          mrb_int va = *(const mrb_int *)a, vb = *(const mrb_int *)b;
          return (va > vb) - (va < vb);
        }
        static sp_IntArray *sp_IntArray_sort(sp_IntArray *a) {
          sp_IntArray *b = sp_IntArray_dup(a);
          qsort(b->data + b->start, b->len, sizeof(mrb_int), _sp_int_cmp);
          return b;
        }
        static void sp_IntArray_sort_bang(sp_IntArray *a) {
          qsort(a->data + a->start, a->len, sizeof(mrb_int), _sp_int_cmp);
        }

        static mrb_int sp_IntArray_length(sp_IntArray *a) {
          return a->len;
        }

        static mrb_int sp_IntArray_get(sp_IntArray *a, mrb_int idx) {
          if (idx < 0) idx += a->len;
          return a->data[a->start + idx];
        }

        static void sp_IntArray_set(sp_IntArray *a, mrb_int idx, mrb_int val) {
          if (idx < 0) idx += a->len;
          if (idx >= 0 && idx < a->len) a->data[a->start + idx] = val;
        }

        static mrb_bool sp_IntArray_neq(sp_IntArray *a, sp_IntArray *b) {
          if (a->len != b->len) return TRUE;
          return memcmp(a->data + a->start, b->data + b->start, sizeof(mrb_int) * a->len) != 0;
        }

        static void sp_IntArray_free(sp_IntArray *a) {
          if (a) { free(a->data); free(a); }
        }

        static const char *sp_IntArray_join(sp_IntArray *a, const char *sep) {
          if (a->len == 0) { char *r = (char *)malloc(1); r[0] = '\\0'; return r; }
          size_t sl = strlen(sep);
          size_t cap = a->len * 24 + (a->len - 1) * sl + 1;
          char *r = (char *)malloc(cap); size_t pos = 0;
          for (mrb_int i = 0; i < a->len; i++) {
            if (i > 0) { memcpy(r + pos, sep, sl); pos += sl; }
            pos += snprintf(r + pos, cap - pos, "%lld", (long long)a->data[a->start + i]);
          }
          return r;
        }

        static sp_IntArray *sp_IntArray_uniq(sp_IntArray *a) {
          sp_IntArray *r = sp_IntArray_new();
          for (mrb_int i = 0; i < a->len; i++) {
            mrb_int v = a->data[a->start + i];
            mrb_bool found = FALSE;
            for (mrb_int j = 0; j < r->len; j++)
              if (r->data[r->start + j] == v) { found = TRUE; break; }
            if (!found) sp_IntArray_push(r, v);
          }
          return r;
        }

        static void sp_IntArray_insert(sp_IntArray *a, mrb_int idx, mrb_int val) {
          if (idx < 0) idx += a->len;
          if (idx < 0) idx = 0;
          if (idx > a->len) idx = a->len;
          if (a->start + a->len >= a->cap) {
            a->cap = (a->cap < 16) ? 16 : a->cap * 2;
            a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap);
          }
          memmove(a->data + a->start + idx + 1, a->data + a->start + idx,
              sizeof(mrb_int) * (a->len - idx));
          a->data[a->start + idx] = val;
          a->len++;
        }

        static sp_IntArray *sp_str_bytes(const char *s) {
          sp_IntArray *a = sp_IntArray_new();
          for (size_t i = 0; s[i]; i++) sp_IntArray_push(a, (unsigned char)s[i]);
          return a;
        }

      C
    end

    def emit_float_array(out)
      out.puts <<~C
        /* ---- Built-in float array ---- */
        typedef struct { mrb_float *data; mrb_int start; mrb_int len; mrb_int cap; } sp_FloatArray;

        static void sp_FloatArray_finalize(void *p) {
          sp_FloatArray *a = (sp_FloatArray *)p;
          free(a->data);
        }

        static sp_FloatArray *sp_FloatArray_new(void) {
          sp_FloatArray *a = (sp_FloatArray *)sp_gc_alloc(sizeof(sp_FloatArray), sp_FloatArray_finalize, NULL);
          a->cap = 16; a->data = (mrb_float *)malloc(sizeof(mrb_float) * a->cap);
          sp_gc_bytes += sizeof(mrb_float) * a->cap;
          return a;
        }

        static sp_FloatArray *sp_FloatArray_dup(sp_FloatArray *a) {
          sp_FloatArray *b = sp_FloatArray_new();
          if (a->len > b->cap) { sp_gc_bytes += sizeof(mrb_float) * (a->len - b->cap); b->cap = a->len; b->data = (mrb_float *)realloc(b->data, sizeof(mrb_float) * b->cap); }
          memcpy(b->data, a->data + a->start, sizeof(mrb_float) * a->len);
          b->len = a->len; return b;
        }

        static void sp_FloatArray_push(sp_FloatArray *a, mrb_float val) {
          mrb_int end = a->start + a->len;
          if (end >= a->cap) {
            if (a->start > 0) { memmove(a->data, a->data + a->start, sizeof(mrb_float) * a->len); a->start = 0; end = a->len; }
            if (end >= a->cap) { a->cap = a->cap * 2 + 1; a->data = (mrb_float *)realloc(a->data, sizeof(mrb_float) * a->cap); }
          }
          a->data[end] = val; a->len++;
        }

        static mrb_float sp_FloatArray_get(sp_FloatArray *a, mrb_int idx) {
          if (idx < 0) idx += a->len;
          return a->data[a->start + idx];
        }

        static void sp_FloatArray_set(sp_FloatArray *a, mrb_int idx, mrb_float val) {
          if (idx < 0) idx += a->len;
          if (idx >= 0 && idx < a->len) a->data[a->start + idx] = val;
        }

        static mrb_int sp_FloatArray_length(sp_FloatArray *a) {
          return a->len;
        }
      C
    end

    def emit_range(out)
      # Range is emitted inline with IntArray if both are needed
      return if @needs_int_array  # already emitted
      out.puts <<~C
        typedef struct { mrb_int first; mrb_int last; } sp_Range;
        static sp_Range sp_Range_new(mrb_int first, mrb_int last) {
          sp_Range r; r.first = first; r.last = last; return r;
        }
        static mrb_bool sp_Range_include_p(sp_Range r, mrb_int v) {
          return v >= r.first && v <= r.last;
        }
      C
    end

    def emit_str_array(out)
      out.puts <<~C
        /* ---- Built-in string array ---- */
        typedef struct { const char **data; mrb_int len; mrb_int cap; } sp_StrArray;

        static sp_StrArray *sp_StrArray_new(void) {
          sp_StrArray *a = (sp_StrArray *)calloc(1, sizeof(sp_StrArray));
          a->cap = 16; a->data = (const char **)malloc(sizeof(const char *) * a->cap);
          return a;
        }

        static void sp_StrArray_push(sp_StrArray *a, const char *s) {
          if (a->len >= a->cap) { a->cap *= 2; a->data = (const char **)realloc(a->data, sizeof(const char *) * a->cap); }
          a->data[a->len++] = s;
        }

        static mrb_int sp_StrArray_length(sp_StrArray *a) {
          return a->len;
        }

        static void sp_StrArray_set(sp_StrArray *a, mrb_int i, const char *v) {
          if (i < 0) i += a->len;
          while (i >= a->len) sp_StrArray_push(a, "");
          a->data[i] = v;
        }

        static const char *sp_StrArray_pop(sp_StrArray *a) {
          if (a->len <= 0) return "";
          return a->data[--a->len];
        }

        static const char *sp_StrArray_last(sp_StrArray *a) {
          if (a->len <= 0) return "";
          return a->data[a->len - 1];
        }

        static sp_StrArray *sp_Dir_glob(const char *pattern) {
          sp_StrArray *a = sp_StrArray_new();
          glob_t g; if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
              char *s = (char *)malloc(strlen(g.gl_pathv[i]) + 1);
              strcpy(s, g.gl_pathv[i]); sp_StrArray_push(a, s);
            } globfree(&g); } return a;
        }

      C
    end

    def emit_str_int_hash(out)
      out.puts <<~C
        /* ---- Built-in string->integer hash table (insertion-ordered) ---- */
        typedef struct sp_HashEntry {
          char *key;
          mrb_int value;
          struct sp_HashEntry *next;
          struct sp_HashEntry *order_next;
          struct sp_HashEntry *order_prev;
        } sp_HashEntry;

        typedef struct {
          sp_HashEntry **buckets;
          mrb_int size;
          mrb_int cap;
          sp_HashEntry *first;
          sp_HashEntry *last;
          mrb_int default_value;
          mrb_bool has_default;
        } sp_StrIntHash;

        static unsigned sp_hash_str(const char *s) {
          unsigned h = 5381;
          while (*s) h = h * 33 + (unsigned char)*s++;
          return h;
        }

        static void sp_StrIntHash_finalize(void *p) {
          sp_StrIntHash *h = (sp_StrIntHash *)p;
          sp_HashEntry *e = h->first;
          while (e) { sp_HashEntry *n = e->order_next; free(e->key); free(e); e = n; }
          free(h->buckets);
        }

        static sp_StrIntHash *sp_StrIntHash_new(void) {
          sp_StrIntHash *h = (sp_StrIntHash *)sp_gc_alloc(sizeof(sp_StrIntHash), sp_StrIntHash_finalize, NULL);
          h->cap = 16; h->size = 0; h->first = NULL; h->last = NULL;
          h->buckets = (sp_HashEntry **)calloc(h->cap, sizeof(sp_HashEntry *));
          return h;
        }

        static mrb_int sp_StrIntHash_set(sp_StrIntHash *h, const char *key, mrb_int value) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_HashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) { e->value = value; return value; }
            e = e->next;
          }
          e = (sp_HashEntry *)malloc(sizeof(sp_HashEntry));
          e->key = (char *)malloc(strlen(key) + 1); strcpy(e->key, key);
          e->value = value;
          e->next = h->buckets[idx];
          h->buckets[idx] = e;
          e->order_next = NULL;
          e->order_prev = h->last;
          if (h->last) h->last->order_next = e; else h->first = e;
          h->last = e;
          h->size++;
          return value;
        }

        static mrb_int sp_StrIntHash_get(sp_StrIntHash *h, const char *key) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_HashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) return e->value;
            e = e->next;
          }
          return h->has_default ? h->default_value : 0;
        }

        static sp_StrIntHash *sp_StrIntHash_new_with_default(mrb_int val) {
          sp_StrIntHash *h = sp_StrIntHash_new();
          h->default_value = val; h->has_default = TRUE;
          return h;
        }

        static mrb_int sp_StrIntHash_length(sp_StrIntHash *h) {
          return h->size;
        }

        static mrb_bool sp_StrIntHash_has_key(sp_StrIntHash *h, const char *key) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_HashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) return TRUE;
            e = e->next;
          }
          return FALSE;
        }

        static mrb_int sp_StrIntHash_delete(sp_StrIntHash *h, const char *key) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_HashEntry **pp = &h->buckets[idx];
          while (*pp) {
            if (strcmp((*pp)->key, key) == 0) {
              sp_HashEntry *e = *pp;
              mrb_int val = e->value;
              *pp = e->next;
              if (e->order_prev) e->order_prev->order_next = e->order_next;
              else h->first = e->order_next;
              if (e->order_next) e->order_next->order_prev = e->order_prev;
              else h->last = e->order_prev;
              free(e->key); free(e);
              h->size--;
              return val;
            }
            pp = &(*pp)->next;
          }
          return 0;
        }

        static sp_IntArray *sp_StrIntHash_values(sp_StrIntHash *h) {
          sp_IntArray *a = sp_IntArray_new();
          sp_HashEntry *e = h->first;
          while (e) { sp_IntArray_push(a, e->value); e = e->order_next; }
          return a;
        }

        static sp_StrIntHash *sp_StrIntHash_merge(sp_StrIntHash *h1, sp_StrIntHash *h2) {
          sp_StrIntHash *r = sp_StrIntHash_new();
          sp_HashEntry *e = h1->first;
          while (e) { sp_StrIntHash_set(r, e->key, e->value); e = e->order_next; }
          e = h2->first;
          while (e) { sp_StrIntHash_set(r, e->key, e->value); e = e->order_next; }
          return r;
        }

      C
    end

    def emit_str_str_hash(out)
      out.puts <<~C
        /* ---- Built-in string->string hash ---- */
        typedef struct sp_RbHashEntry {
          char *key;
          char *value;
          struct sp_RbHashEntry *next;
          struct sp_RbHashEntry *order_next;
        } sp_RbHashEntry;

        typedef struct {
          sp_RbHashEntry **buckets;
          mrb_int size;
          mrb_int cap;
          sp_RbHashEntry *first;
          sp_RbHashEntry *last;
        } sp_RbHash;

        static sp_RbHash *sp_RbHash_new(void) {
          sp_RbHash *h = (sp_RbHash *)calloc(1, sizeof(sp_RbHash));
          h->cap = 16; h->size = 0; h->first = NULL; h->last = NULL;
          h->buckets = (sp_RbHashEntry **)calloc(h->cap, sizeof(sp_RbHashEntry *));
          return h;
        }

        static const char *sp_RbHash_set(sp_RbHash *h, const char *key, const char *value) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_RbHashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) { free(e->value); e->value = strdup(value); return value; }
            e = e->next;
          }
          e = (sp_RbHashEntry *)calloc(1, sizeof(sp_RbHashEntry));
          e->key = strdup(key); e->value = strdup(value);
          e->next = h->buckets[idx]; h->buckets[idx] = e;
          e->order_next = NULL;
          if (h->last) h->last->order_next = e; else h->first = e;
          h->last = e;
          h->size++;
          return value;
        }

        static const char *sp_RbHash_get(sp_RbHash *h, const char *key) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_RbHashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) return e->value;
            e = e->next;
          }
          return "";
        }

        static mrb_int sp_RbHash_length(sp_RbHash *h) { return h->size; }

      C
    end

    def emit_poly_hash(out)
      out.puts <<~C
        /* ---- Built-in string->sp_RbValue hash ---- */
        typedef struct sp_PolyHashEntry {
          char *key;
          sp_RbValue value;
          struct sp_PolyHashEntry *next;
          struct sp_PolyHashEntry *order_next;
        } sp_PolyHashEntry;

        typedef struct {
          sp_PolyHashEntry **buckets;
          mrb_int size;
          mrb_int cap;
          sp_PolyHashEntry *first;
          sp_PolyHashEntry *last;
        } sp_PolyHash;

        static sp_PolyHash *sp_PolyHash_new(void) {
          sp_PolyHash *h = (sp_PolyHash *)calloc(1, sizeof(sp_PolyHash));
          h->cap = 16; h->size = 0; h->first = NULL; h->last = NULL;
          h->buckets = (sp_PolyHashEntry **)calloc(h->cap, sizeof(sp_PolyHashEntry *));
          return h;
        }

        static sp_RbValue sp_PolyHash_set(sp_PolyHash *h, const char *key, sp_RbValue value) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_PolyHashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) { e->value = value; return value; }
            e = e->next;
          }
          e = (sp_PolyHashEntry *)calloc(1, sizeof(sp_PolyHashEntry));
          e->key = strdup(key); e->value = value;
          e->next = h->buckets[idx]; h->buckets[idx] = e;
          e->order_next = NULL;
          if (h->last) h->last->order_next = e; else h->first = e;
          h->last = e;
          h->size++;
          return value;
        }

        static sp_RbValue sp_PolyHash_get(sp_PolyHash *h, const char *key) {
          unsigned idx = sp_hash_str(key) % h->cap;
          sp_PolyHashEntry *e = h->buckets[idx];
          while (e) {
            if (strcmp(e->key, key) == 0) return e->value;
            e = e->next;
          }
          return sp_box_nil();
        }

        static mrb_int sp_PolyHash_length(sp_PolyHash *h) { return h->size; }

      C
    end

    def emit_mutable_string(out)
      out.puts <<~C
        /* ---- Mutable string ---- */
        typedef struct { char *data; int64_t len; int64_t cap; } sp_String;
        static sp_String *sp_String_new(const char *s) {
          sp_String *r = (sp_String *)malloc(sizeof(sp_String));
          r->len = (int64_t)strlen(s);
          r->cap = r->len < 16 ? 16 : r->len * 2;
          r->data = (char *)malloc(r->cap + 1);
          memcpy(r->data, s, r->len + 1);
          return r;
        }
        static sp_String *sp_String_new_empty(void) { return sp_String_new(""); }
        static void sp_String_append(sp_String *s, const char *t) {
          int64_t tl = (int64_t)strlen(t);
          if (s->len + tl >= s->cap) {
            s->cap = (s->len + tl) * 2;
            s->data = (char *)realloc(s->data, s->cap + 1);
          }
          memcpy(s->data + s->len, t, tl + 1);
          s->len += tl;
        }
        static const char *sp_String_cstr(sp_String *s) { return s->data; }
        static int64_t sp_String_length(sp_String *s) { return s->len; }
        static void sp_String_replace(sp_String *s, const char *t) {
          size_t tlen = strlen(t);
          if (tlen >= (size_t)s->cap) { s->cap = tlen + 1; s->data = realloc(s->data, s->cap); }
          memcpy(s->data, t, tlen + 1); s->len = tlen;
        }
        static void sp_String_clear(sp_String *s) {
          s->data[0] = '\\0'; s->len = 0;
        }
        static const char *sp_String_char_at(sp_String *s, mrb_int idx) {
          if (idx < 0) idx = s->len + idx;
          if (idx < 0 || idx >= s->len) return "";
          char *r = (char *)malloc(2); r[0] = s->data[idx]; r[1] = '\\0'; return r;
        }
        static sp_String *sp_String_dup(sp_String *s) { return sp_String_new(s->data); }
        static void sp_String_setbyte(sp_String *s, mrb_int i, mrb_int b) {
          if (i >= 0 && i < s->len) s->data[i] = (char)b;
        }
        static void sp_String_append_str(sp_String *s, sp_String *t) {
          sp_String_append(s, t->data);
        }

      C
    end
  end

  # ---- Main entry point ----
  def self.run(args)
    source_file = nil
    output_file = nil

    args.each do |arg|
      if arg.start_with?("--source=")
        source_file = arg.split("=", 2).last
      elsif arg.start_with?("--output=")
        output_file = arg.split("=", 2).last
      end
    end

    unless source_file
      $stderr.puts "Usage: ruby spinel.rb --source=input.rb --output=output.c"
      exit 1
    end

    source = File.read(source_file)
    compiler = Compiler.new(source, source_file)
    c_code = compiler.compile

    if output_file
      File.write(output_file, c_code)
      $stderr.puts "Wrote #{output_file}"
    else
      puts c_code
    end
  end
end

Spinel.run(ARGV)
