#!/usr/bin/env ruby
# Spinel AOT Compiler (Ruby implementation)
#
# Compiles Ruby source to standalone C via Prism AST and type inference.

require "prism"

module Spinel
  # Type system
  module Type
    UNKNOWN  = :unknown
    INTEGER  = :integer
    FLOAT    = :float
    BOOLEAN  = :boolean
    STRING   = :string
    NIL      = :nil
    OBJECT   = :object
    ARRAY    = :array
    HASH     = :hash
    VOID     = :void
  end

  # Variable entry
  VarEntry = Struct.new(:name, :type, :declared, :is_constant, keyword_init: true)

  # Method info
  MethodInfo = Struct.new(:name, :params, :body_node, :return_type, :is_class_method, keyword_init: true)

  # Parameter info
  ParamInfo = Struct.new(:name, :type, :is_optional, :default_node, keyword_init: true)

  # Class info
  ClassInfo = Struct.new(:name, :superclass, :methods, :ivars, :node, keyword_init: true) {
    def find_method(name)
      methods.find { |m| m.name == name }
    end
  }

  # Code generation context
  class Context
    attr_accessor :out, :indent, :vars, :classes, :funcs,
                  :temp_counter, :current_class, :current_method

    def initialize(out)
      @out = out
      @indent = 0
      @vars = []
      @classes = []
      @funcs = []
      @temp_counter = 0
      @current_class = nil
      @current_method = nil
    end

    def emit(str)
      @out.print("  " * @indent)
      @out.puts(str)
    end

    def emit_raw(str)
      @out.puts(str)
    end

    def var_lookup(name)
      @vars.reverse.find { |v| v.name == name }
    end

    def var_declare(name, type)
      existing = var_lookup(name)
      if existing
        existing.type = type
        return existing
      end
      v = VarEntry.new(name: name, type: type, declared: false, is_constant: false)
      @vars.push(v)
      v
    end

    def find_class(name)
      @classes.find { |c| c.name == name }
    end

    def next_temp
      t = @temp_counter
      @temp_counter += 1
      t
    end
  end

  # Type inference
  class TypeInferrer
    def initialize(ctx)
      @ctx = ctx
    end

    def infer(node)
      case node
      when Prism::IntegerNode
        Type::INTEGER
      when Prism::FloatNode
        Type::FLOAT
      when Prism::StringNode, Prism::InterpolatedStringNode
        Type::STRING
      when Prism::TrueNode, Prism::FalseNode
        Type::BOOLEAN
      when Prism::NilNode
        Type::NIL
      when Prism::LocalVariableReadNode
        v = @ctx.var_lookup(node.name.to_s)
        v ? v.type : Type::UNKNOWN
      when Prism::CallNode
        infer_call(node)
      when Prism::LocalVariableWriteNode
        infer(node.value)
      when Prism::ParenthesesNode
        if node.body && node.body.is_a?(Prism::StatementsNode) && !node.body.body.empty?
          infer(node.body.body.last)
        else
          Type::NIL
        end
      else
        Type::UNKNOWN
      end
    end

    def infer_call(node)
      name = node.name.to_s

      # Constructor: ClassName.new(...)
      if name == "new" && node.receiver.is_a?(Prism::ConstantReadNode)
        cls_name = node.receiver.name.to_s
        return [:object, cls_name] if @ctx.find_class(cls_name)
      end

      if node.receiver
        recv_t = infer(node.receiver)
        # Built-in method return types
        case recv_t
        when Type::INTEGER
          return Type::INTEGER if ["+", "-", "*", "/", "%", "**", "abs", "succ"].include?(name)
          return Type::FLOAT if name == "to_f"
          return Type::STRING if name == "to_s"
          return Type::BOOLEAN if ["even?", "odd?", "zero?", "positive?", "negative?"].include?(name)
        when Type::FLOAT
          return Type::FLOAT if ["+", "-", "*", "/", "abs"].include?(name)
          return Type::INTEGER if ["ceil", "floor", "round", "to_i"].include?(name)
          return Type::STRING if name == "to_s"
        when Type::STRING
          return Type::INTEGER if ["length", "size", "to_i", "index", "count"].include?(name)
          return Type::STRING if ["+", "upcase", "downcase", "strip", "chomp", "reverse", "gsub", "sub"].include?(name)
          return Type::BOOLEAN if ["empty?", "include?", "start_with?", "end_with?"].include?(name)
        when Type::ARRAY
          return Type::INTEGER if ["length", "size", "first", "last", "pop", "shift", "sum", "min", "max"].include?(name)
          return Type::ARRAY if ["push", "<<", "sort", "reverse", "map", "select", "reject", "dup"].include?(name)
          return Type::BOOLEAN if ["empty?", "include?", "any?", "all?", "none?"].include?(name)
          return Type::STRING if name == "join"
        end

        # OBJECT type: [:object, "ClassName"]
        if recv_t.is_a?(Array) && recv_t[0] == :object
          cls = @ctx.find_class(recv_t[1])
          if cls
            m = cls.find_method(name)
            return m.return_type if m && m.return_type != Type::UNKNOWN
            # Default: same object type for chaining, INTEGER otherwise
            return Type::INTEGER
          end
        end
        return Type::BOOLEAN if ["==", "!=", "<", ">", "<=", ">=", "nil?", "is_a?"].include?(name)
      else
        # Kernel methods
        return Type::NIL if ["puts", "print", "p"].include?(name)
        return Type::STRING if name == "gets"
        return Type::INTEGER if name == "rand"
      end
      Type::UNKNOWN
    end
  end

  # Class analysis pass
  class ClassAnalyzer
    def initialize(ctx)
      @ctx = ctx
      @inferrer = TypeInferrer.new(ctx)
    end

    def analyze(node)
      return unless node.is_a?(Prism::ProgramNode)
      node.statements.body.each { |s| analyze_stmt(s) }
    end

    def analyze_stmt(node)
      case node
      when Prism::ClassNode
        analyze_class(node)
      when Prism::DefNode
        analyze_top_func(node)
      end
    end

    def analyze_class(node)
      name = node.constant_path.name.to_s
      superclass = ""
      if node.superclass.is_a?(Prism::ConstantReadNode)
        superclass = node.superclass.name.to_s
      end

      cls = ClassInfo.new(
        name: name, superclass: superclass,
        methods: [], ivars: [], node: node
      )

      if node.body && node.body.is_a?(Prism::StatementsNode)
        node.body.body.each do |s|
          if s.is_a?(Prism::DefNode)
            m = analyze_method(s)
            cls.methods.push(m)
          end
        end
      end

      @ctx.classes.push(cls)
    end

    def analyze_method(node)
      params = []
      if node.parameters
        node.parameters.requireds.each do |p|
          if p.is_a?(Prism::RequiredParameterNode)
            params.push(ParamInfo.new(name: p.name.to_s, type: Type::UNKNOWN))
          end
        end
        node.parameters.optionals.each do |p|
          if p.is_a?(Prism::OptionalParameterNode)
            params.push(ParamInfo.new(
              name: p.name.to_s, type: @inferrer.infer(p.value),
              is_optional: true, default_node: p.value
            ))
          end
        end
      end

      MethodInfo.new(
        name: node.name.to_s,
        params: params,
        body_node: node.body,
        return_type: Type::UNKNOWN,
        is_class_method: false
      )
    end

    def analyze_top_func(node)
      m = analyze_method(node)
      @ctx.funcs.push(m)
    end
  end

  # Expression code generation
  class ExprCodegen
    def initialize(ctx)
      @ctx = ctx
      @inferrer = TypeInferrer.new(ctx)
    end

    def generate(node)
      case node
      when Prism::IntegerNode
        node.value.to_s
      when Prism::FloatNode
        s = "%.17g" % node.value
        s += ".0" unless s.include?(".") || s.include?("e")
        s
      when Prism::StringNode
        c_string_literal(node.content)
      when Prism::InterpolatedStringNode
        parts = node.parts.map do |part|
          case part
          when Prism::StringNode
            c_string_literal(part.content)
          when Prism::EmbeddedStatementsNode
            if part.statements && !part.statements.body.empty?
              expr = generate(part.statements.body.first)
              t = @inferrer.infer(part.statements.body.first)
              case t
              when Type::INTEGER then "sp_int_to_s(#{expr})"
              when Type::FLOAT then "sp_float_to_s(#{expr})"
              else expr
              end
            else
              '""'
            end
          else
            '""'
          end
        end
        if parts.length == 1
          parts.first
        else
          parts.reduce { |a, b| "sp_str_concat(#{a}, #{b})" }
        end
      when Prism::TrueNode
        "TRUE"
      when Prism::FalseNode
        "FALSE"
      when Prism::NilNode
        "0 /* nil */"
      when Prism::LocalVariableReadNode
        "lv_#{node.name}"
      when Prism::LocalVariableWriteNode
        val = generate(node.value)
        t = @inferrer.infer(node.value)
        @ctx.var_declare(node.name.to_s, t)
        "(lv_#{node.name} = #{val})"
      when Prism::InstanceVariableReadNode
        name = node.name.to_s.delete_prefix("@")
        "self->#{name}"
      when Prism::InstanceVariableWriteNode
        name = node.name.to_s.delete_prefix("@")
        val = generate(node.value)
        "(self->#{name} = #{val})"
      when Prism::CallNode
        generate_call(node)
      when Prism::ParenthesesNode
        if node.body && node.body.is_a?(Prism::StatementsNode) && !node.body.body.empty?
          "(#{generate(node.body.body.last)})"
        else
          "0"
        end
      when Prism::IfNode
        cond = generate(node.predicate)
        then_e = node.statements ? generate(node.statements.body.last) : "0"
        else_e = node.subsequent ? generate_if_else(node.subsequent) : "0"
        "(#{cond} ? #{then_e} : #{else_e})"
      when Prism::AndNode
        "(#{generate(node.left)} && #{generate(node.right)})"
      when Prism::OrNode
        "(#{generate(node.left)} || #{generate(node.right)})"
      when Prism::ArrayNode
        # For now, handle simple integer arrays
        elements = node.elements.map { |e| generate(e) }
        tmp = @ctx.next_temp
        @ctx.emit("sp_IntArray *_ary_#{tmp} = sp_IntArray_new();")
        elements.each { |e| @ctx.emit("sp_IntArray_push(_ary_#{tmp}, #{e});") }
        "_ary_#{tmp}"
      when Prism::RangeNode
        left = generate(node.left)
        right = generate(node.right)
        "sp_Range_new(#{left}, #{right})"
      when Prism::SymbolNode
        c_string_literal(node.value)
      when Prism::SelfNode
        "self"
      else
        "0 /* unsupported: #{node.class.name.split("::").last} */"
      end
    end

    def generate_if_else(node)
      case node
      when Prism::ElseNode
        node.statements ? generate(node.statements.body.last) : "0"
      when Prism::IfNode
        cond = generate(node.predicate)
        then_e = node.statements ? generate(node.statements.body.last) : "0"
        else_e = node.subsequent ? generate_if_else(node.subsequent) : "0"
        "(#{cond} ? #{then_e} : #{else_e})"
      else
        "0"
      end
    end

    def generate_call(node)
      name = node.name.to_s
      args = node.arguments ? node.arguments.arguments : []

      # Constructor: ClassName.new(args)
      if name == "new" && node.receiver.is_a?(Prism::ConstantReadNode)
        cls_name = node.receiver.name.to_s
        cls = @ctx.find_class(cls_name)
        if cls
          tmp = @ctx.next_temp
          @ctx.emit("sp_#{cls_name} *_obj_#{tmp} = (sp_#{cls_name} *)calloc(1, sizeof(sp_#{cls_name}));")
          init = cls.find_method("initialize")
          if init
            arg_strs = args.map { |a| generate(a) }
            all_args = (["_obj_#{tmp}"] + arg_strs).join(", ")
            @ctx.emit("sp_#{cls_name}_initialize(#{all_args});")
          end
          return "_obj_#{tmp}"
        end
      end

      if node.receiver
        recv = generate(node.receiver)
        recv_t = @inferrer.infer(node.receiver)

        # Binary operators
        if args.length == 1 && ["+", "-", "*", "/", "%", "**", "<", ">", "<=", ">=", "==", "!=", "&", "|", "^", "<<", ">>"].include?(name)
          arg = generate(args.first)
          if name == "**"
            return "((mrb_int)pow((double)#{recv}, (double)#{arg}))"
          end
          return "(#{recv} #{name} #{arg})"
        end

        # Unary operators
        return "(-#{recv})" if name == "-@" && args.empty?
        return "(~#{recv})" if name == "~" && args.empty?

        # String methods
        if recv_t == Type::STRING
          case name
          when "length", "size" then return "((mrb_int)strlen(#{recv}))"
          when "empty?" then return "(strlen(#{recv}) == 0)"
          when "upcase" then return "sp_str_upcase(#{recv})"
          when "downcase" then return "sp_str_downcase(#{recv})"
          when "strip" then return "sp_str_strip(#{recv})"
          when "chomp" then return "sp_str_chomp(#{recv})"
          when "reverse" then return "sp_str_reverse(#{recv})"
          when "to_i" then return "((mrb_int)strtol(#{recv}, NULL, 10))"
          when "include?"
            arg = generate(args.first)
            return "(strstr(#{recv}, #{arg}) != NULL)"
          when "gsub"
            from = generate(args[0])
            to = generate(args[1])
            return "sp_str_gsub(#{recv}, #{from}, #{to})"
          end
        end

        # Integer methods
        if recv_t == Type::INTEGER
          case name
          when "abs" then return "(#{recv} < 0 ? -#{recv} : #{recv})"
          when "even?" then return "(#{recv} % 2 == 0)"
          when "odd?" then return "(#{recv} % 2 != 0)"
          when "zero?" then return "(#{recv} == 0)"
          when "to_f" then return "((mrb_float)#{recv})"
          when "to_s" then return "sp_int_to_s(#{recv})"
          when "times" then return recv  # handled in stmt
          end
        end

        # Array methods
        if recv_t == Type::ARRAY
          case name
          when "length", "size" then return "sp_IntArray_length(#{recv})"
          when "push", "<<"
            arg = generate(args.first)
            return "(sp_IntArray_push(#{recv}, #{arg}), (mrb_int)0)"
          when "[]"
            arg = generate(args.first)
            return "sp_IntArray_get(#{recv}, #{arg})"
          when "first" then return "sp_IntArray_get(#{recv}, 0)"
          when "last" then return "sp_IntArray_get(#{recv}, sp_IntArray_length(#{recv}) - 1)"
          when "empty?" then return "(sp_IntArray_length(#{recv}) == 0)"
          end
        end

        # Object method calls (user-defined classes)
        if recv_t.is_a?(Array) && recv_t[0] == :object
          cls_name = recv_t[1]
          arg_strs = args.map { |a| generate(a) }
          all_args = ([recv] + arg_strs).join(", ")
          return "sp_#{cls_name}_#{c_safe(name)}(#{all_args})"
        end

        # Fallback
        arg_strs = args.map { |a| generate(a) }
        "0 /* unsupported: #{name} */"
      else
        # Kernel methods
        case name
        when "puts"
          if args.empty?
            @ctx.emit("putchar('\\n');")
            return "0"
          end
          arg = generate(args.first)
          arg_t = @inferrer.infer(args.first)
          case arg_t
          when Type::INTEGER
            @ctx.emit("printf(\"%lld\\n\", (long long)#{arg});")
          when Type::FLOAT
            @ctx.emit("printf(\"%g\\n\", #{arg});")
          when Type::BOOLEAN
            @ctx.emit("puts(#{arg} ? \"true\" : \"false\");")
          when Type::STRING
            @ctx.emit("{ const char *_ps = #{arg}; fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }")
          else
            @ctx.emit("printf(\"%lld\\n\", (long long)#{arg});")
          end
          "0"
        when "print"
          arg = generate(args.first)
          @ctx.emit("fputs(#{arg}, stdout);")
          "0"
        when "p"
          arg = generate(args.first)
          @ctx.emit("printf(\"%lld\\n\", (long long)#{arg});")
          "0"
        when "rand"
          "((mrb_int)rand())"
        when "exit"
          code = args.empty? ? "0" : generate(args.first)
          @ctx.emit("exit(#{code});")
          "0"
        when "raise"
          msg = args.empty? ? '""' : generate(args.first)
          @ctx.emit("fprintf(stderr, \"%s\\n\", #{msg}); exit(1);")
          "0"
        else
          # User-defined function call
          arg_strs = args.map { |a| generate(a) }
          "sp_#{c_safe(name)}(#{arg_strs.join(", ")})"
        end
      end
    end

    private

    def c_string_literal(s)
      '"' + s.gsub("\\", "\\\\\\\\").gsub('"', '\\"').gsub("\n", "\\n").gsub("\t", "\\t") + '"'
    end

    def c_safe(name)
      name.gsub("?", "_p").gsub("!", "_bang").gsub("=", "_eq")
    end
  end

  # Statement code generation
  class StmtCodegen
    def initialize(ctx)
      @ctx = ctx
      @expr = ExprCodegen.new(ctx)
      @inferrer = TypeInferrer.new(ctx)
    end

    def generate(node)
      case node
      when Prism::LocalVariableWriteNode
        val = @expr.generate(node.value)
        t = @inferrer.infer(node.value)
        v = @ctx.var_declare(node.name.to_s, t)
        unless v.declared
          ctype = c_type(t)
          @ctx.emit("#{ctype} lv_#{node.name} = #{val};")
          v.declared = true
        else
          @ctx.emit("lv_#{node.name} = #{val};")
        end
      when Prism::IfNode
        generate_if(node)
      when Prism::UnlessNode
        cond = @expr.generate(node.predicate)
        @ctx.emit("if (!(#{cond})) {")
        @ctx.indent += 1
        generate_body(node.statements)
        @ctx.indent -= 1
        if node.else_clause
          @ctx.emit("}")
          @ctx.emit("else {")
          @ctx.indent += 1
          generate_body(node.else_clause.statements)
          @ctx.indent -= 1
        end
        @ctx.emit("}")
      when Prism::WhileNode
        cond = @expr.generate(node.predicate)
        @ctx.emit("while (#{cond}) {")
        @ctx.indent += 1
        generate_body(node.statements)
        @ctx.indent -= 1
        @ctx.emit("}")
      when Prism::CallNode
        generate_call_stmt(node)
      when Prism::ReturnNode
        if node.arguments && !node.arguments.arguments.empty?
          val = @expr.generate(node.arguments.arguments.first)
          @ctx.emit("return #{val};")
        else
          @ctx.emit("return;")
        end
      when Prism::BreakNode
        @ctx.emit("break;")
      when Prism::NextNode
        @ctx.emit("continue;")
      when Prism::StatementsNode
        node.body.each { |s| generate(s) }
      when Prism::ClassNode, Prism::DefNode, Prism::ModuleNode
        # Handled by class analysis / emit passes
      when Prism::InstanceVariableWriteNode
        name = node.name.to_s.delete_prefix("@")
        val = @expr.generate(node.value)
        @ctx.emit("self->#{name} = #{val};")
      else
        # Try as expression
        expr = @expr.generate(node)
        @ctx.emit("#{expr};") unless expr == "0" || expr.start_with?("0 /*")
      end
    end

    def generate_if(node)
      cond = @expr.generate(node.predicate)
      @ctx.emit("if (#{cond}) {")
      @ctx.indent += 1
      generate_body(node.statements)
      @ctx.indent -= 1
      if node.subsequent
        case node.subsequent
        when Prism::ElseNode
          @ctx.emit("}")
          @ctx.emit("else {")
          @ctx.indent += 1
          generate_body(node.subsequent.statements)
          @ctx.indent -= 1
        when Prism::IfNode
          @ctx.emit("}")
          @ctx.emit("else ")
          generate_if(node.subsequent)
          return  # don't emit closing brace
        end
      end
      @ctx.emit("}")
    end

    def generate_call_stmt(node)
      expr = @expr.generate(node)
      # If the expression wasn't already emitted as a side-effect (puts etc.),
      # emit it as a statement
      unless expr == "0" || expr.start_with?("0 /*")
        @ctx.emit("#{expr};")
      end
    end

    def generate_body(stmts_node)
      return unless stmts_node
      if stmts_node.is_a?(Prism::StatementsNode)
        stmts_node.body.each { |s| generate(s) }
      else
        generate(stmts_node)
      end
    end

    private

    def c_type(t)
      if t.is_a?(Array) && t[0] == :object
        return "sp_#{t[1]} *"
      end
      case t
      when Type::INTEGER then "mrb_int"
      when Type::FLOAT then "mrb_float"
      when Type::BOOLEAN then "mrb_bool"
      when Type::STRING then "const char *"
      when Type::ARRAY then "sp_IntArray *"
      else "mrb_int"
      end
    end
  end

  # Main compiler
  class Compiler
    def initialize(source, source_path)
      @source = source
      @source_path = source_path
    end

    def compile(output_path)
      result = Prism.parse(@source)
      unless result.success?
        result.errors.each { |e| $stderr.puts "#{@source_path}:#{e.location.start_line}: #{e.message}" }
        exit 1
      end

      prog = result.value
      out = File.open(output_path, "w")
      ctx = Context.new(out)

      # Pass 1: Class analysis
      analyzer = ClassAnalyzer.new(ctx)
      analyzer.analyze(prog)

      # Pass 2: Emit header
      emit_header(out)

      # Pass 3: Emit class structs and methods
      ctx.classes.each do |cls|
        emit_struct(out, cls)
        emit_methods(out, ctx, cls)
      end

      # Pass 4: Emit top-level functions
      ctx.funcs.each do |fn|
        emit_function(out, ctx, fn)
      end

      # Pass 5: Emit main
      emit_main(out, ctx, prog)

      out.close
      $stderr.puts "Wrote #{output_path}"
    end

    private

    def emit_header(out)
      out.puts "/* Generated by Spinel AOT compiler (Ruby edition) */"
      out.puts '#include <stdio.h>'
      out.puts '#include <stdlib.h>'
      out.puts '#include <string.h>'
      out.puts '#include <math.h>'
      out.puts '#include <stdbool.h>'
      out.puts '#include <stdint.h>'
      out.puts ""
      out.puts "typedef int64_t mrb_int;"
      out.puts "typedef double mrb_float;"
      out.puts "typedef bool mrb_bool;"
      out.puts "#define TRUE true"
      out.puts "#define FALSE false"
      out.puts ""
      # String helpers
      out.puts 'static const char *sp_str_concat(const char *a, const char *b) {'
      out.puts '  size_t la = strlen(a), lb = strlen(b);'
      out.puts '  char *r = (char *)malloc(la + lb + 1);'
      out.puts '  memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r;'
      out.puts '}'
      out.puts 'static const char *sp_int_to_s(mrb_int n) {'
      out.puts '  char *r = (char *)malloc(24); snprintf(r, 24, "%lld", (long long)n); return r;'
      out.puts '}'
      out.puts 'static const char *sp_float_to_s(mrb_float f) {'
      out.puts '  char *r = (char *)malloc(32); snprintf(r, 32, "%g", f); return r;'
      out.puts '}'
      out.puts 'static const char *sp_str_upcase(const char *s) {'
      out.puts '  size_t n = strlen(s); char *r = (char *)malloc(n + 1);'
      out.puts '  for (size_t i = 0; i <= n; i++) r[i] = toupper((unsigned char)s[i]); return r;'
      out.puts '}'
      out.puts 'static const char *sp_str_downcase(const char *s) {'
      out.puts '  size_t n = strlen(s); char *r = (char *)malloc(n + 1);'
      out.puts '  for (size_t i = 0; i <= n; i++) r[i] = tolower((unsigned char)s[i]); return r;'
      out.puts '}'
      out.puts ""
    end

    def emit_struct(out, cls)
      out.puts "typedef struct sp_#{cls.name}_s sp_#{cls.name};"
      out.puts "struct sp_#{cls.name}_s {"
      # Collect ivars from initialize method
      init = cls.find_method("initialize")
      if init && init.body_node
        scan_ivars(init.body_node).each do |ivar|
          out.puts "  mrb_int #{ivar}; /* TODO: infer type */"
        end
      end
      out.puts "};"
      out.puts ""
    end

    def scan_ivars(node)
      ivars = []
      case node
      when Prism::InstanceVariableWriteNode
        ivars.push(node.name.to_s.delete_prefix("@"))
      when Prism::StatementsNode
        node.body.each { |s| ivars.concat(scan_ivars(s)) }
      end
      ivars.uniq
    end

    def emit_methods(out, ctx, cls)
      cls.methods.each do |m|
        if m.name == "initialize"
          # Emit as void sp_ClassName_initialize(sp_ClassName *self, args...)
          param_str = (["sp_#{cls.name} *self"] + m.params.map { |p| "mrb_int lv_#{p.name}" }).join(", ")
          out.puts "static void sp_#{cls.name}_initialize(#{param_str}) {"
          if m.body_node
            stmt_gen = StmtCodegen.new(ctx)
            ctx.indent = 1
            ctx.current_class = cls
            if m.body_node.is_a?(Prism::StatementsNode)
              m.body_node.body.each { |s| stmt_gen.generate(s) }
            end
            ctx.current_class = nil
          end
          out.puts "}"
          out.puts ""
          next
        end
        param_str = (["sp_#{cls.name} *self"] + m.params.map { |p| "mrb_int lv_#{p.name}" }).join(", ")
        out.puts "static mrb_int sp_#{cls.name}_#{c_safe(m.name)}(#{param_str}) {"
        if m.body_node
          stmt_gen = StmtCodegen.new(ctx)
          ctx.indent = 1
          ctx.current_class = cls
          generate_method_body(ctx, m, stmt_gen)
          ctx.current_class = nil
        end
        out.puts "}"
        out.puts ""
      end
    end

    def generate_method_body(ctx, method, stmt_gen)
      return unless method.body_node
      if method.body_node.is_a?(Prism::StatementsNode)
        stmts = method.body_node.body
        stmts.each_with_index do |s, i|
          if i == stmts.length - 1
            # Last expression is implicit return
            expr = ExprCodegen.new(ctx)
            val = expr.generate(s)
            ctx.emit("return #{val};")
          else
            stmt_gen.generate(s)
          end
        end
      else
        expr = ExprCodegen.new(ctx)
        val = expr.generate(method.body_node)
        ctx.emit("return #{val};")
      end
    end

    def emit_function(out, ctx, fn)
      param_str = fn.params.map { |p| "mrb_int lv_#{p.name}" }.join(", ")
      param_str = "void" if param_str.empty?
      out.puts "static mrb_int sp_#{c_safe(fn.name)}(#{param_str}) {"
      if fn.body_node
        stmt_gen = StmtCodegen.new(ctx)
        ctx.indent = 1
        generate_method_body(ctx, fn, stmt_gen)
      end
      out.puts "}"
      out.puts ""
    end

    def emit_main(out, ctx, prog)
      out.puts "int main(int argc, char **argv) {"
      ctx.indent = 1
      stmt_gen = StmtCodegen.new(ctx)
      prog.statements.body.each do |s|
        next if s.is_a?(Prism::ClassNode) || s.is_a?(Prism::DefNode) || s.is_a?(Prism::ModuleNode)
        stmt_gen.generate(s)
      end
      ctx.emit("return 0;")
      out.puts "}"
      out.puts ""
    end

    def c_safe(name)
      name.gsub("?", "_p").gsub("!", "_bang").gsub("=", "_eq")
    end
  end
end

# CLI
if ARGV.length < 2
  $stderr.puts "Usage: ruby spinel.rb --source=INPUT.rb --output=OUTPUT.c"
  exit 1
end

source_path = nil
output_path = nil
ARGV.each do |arg|
  if arg.start_with?("--source=")
    source_path = arg.delete_prefix("--source=")
  elsif arg.start_with?("--output=")
    output_path = arg.delete_prefix("--output=")
  end
end

unless source_path && output_path
  $stderr.puts "Usage: ruby spinel.rb --source=INPUT.rb --output=OUTPUT.c"
  exit 1
end

source = File.read(source_path)
compiler = Spinel::Compiler.new(source, source_path)
compiler.compile(output_path)
