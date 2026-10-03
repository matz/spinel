# Edits that never run, for the dead code probe (see tools/dead_code_probe.rb).
#
#   sites = DeadCodeEdit.sites(source, alien: "sym")
#   DeadCodeEdit.apply(source, sites.select { |s| s.kind == "local" })
#
# The probe asks whether a program still prints its answer after code that
# never runs is added to it. This file is the half that needs only Prism: it
# finds the places such code can go (a Site each) and writes the program
# again with any set of them filled in.
#
# Every edit is a statement under one guard, `EDIT if ::ARGV.length == 9123`,
# which is false in every run and which the compiler cannot fold (and
# which names `::ARGV`, so that a method of a BasicObject can read it):
# Spinel types a slot from every write it can see, reached or not, so the
# edit changes what the compiler decides and nothing the program does. The
# value an edit writes is the alien: a value of a class the slot most
# likely does not hold (ALIENS; `:dead` unless another is asked for).
#
# The kinds, each named for the decision it takes from the compiler:
#
#   local    `x = ALIEN` ahead of a local's first write: the local is boxed
#   ivar     `@x = ALIEN` ahead of an instance variable's first write in a
#            class (or in the program, or in a class's own methods)
#   gvar     `$x = ALIEN` ahead of a global's first write
#   elem     `x << ALIEN`, or `x[ALIEN] = ALIEN`, after a variable's first
#            write of an array or hash literal: its elements are boxed
#   arg      a call of the method with one positional argument the alien,
#            at the top of its own body (`m(a, ALIEN)`; `C.new(ALIEN, b)`
#            in a class's initialize): the parameter is boxed
#   return   `return ALIEN` at the top of a method: its value is boxed
#   next     `next ALIEN` at the top of a block or lambda: its value is boxed
#   capture  a lambda reading a local or a parameter, stored in a global
#            of its own: the variable lives in a cell
#   escape   a local or a parameter stored in a global of its own: its
#            value outlives the frame
#   raise    `raise "dead"` at the top of a method: the method may raise
#   nop      `nil` at the top of a method: the control, which changes
#            nothing but the presence of the guard
#
# Where an edit goes. A statement ahead of a write stands directly in a
# body (a StatementsNode), in front of the write; one that reads the
# variable stands in front of the statement that follows the write, and is
# left out when the write ends its body, whose value it would become. A
# body that is one expression by its grammar takes no statement: the arm of
# a ternary, an interpolation, and the body of `def m = expr`, which takes
# an edit at its top only inside parentheses put around it. First write
# means first in the source among the writes that are statements, within a
# method or a class body (a local first written in a block is the block's
# own); `a, b = 1, 2` and `x += 1` and a write that is an argument are not
# edited.
#
# An edit goes on the line of the statement it stands by, so no line of
# the program moves: `__LINE__` and a backtrace answer as before.
#
# Sources are binary Strings and offsets are bytes, as in order_permute.rb.

require "prism"

module DeadCodeEdit
  # Prism reported errors for the source.
  class ParseError < StandardError; end

  # One place an edit can go. kind is one of KINDS; name the slot in words
  # (`x`, `@x`, `C#m(a)`, `C#m`, `block`); line the line the edit lands on;
  # at the [line, column, node kind] of the record that carries the slot's
  # type in an --emit-types dump of the unedited program, or nil; inserts
  # the [byte offset, text] pairs that make the edit.
  Site = Struct.new(:id, :kind, :name, :line, :at, :inserts)

  GUARD = "::ARGV.length == 9123".freeze
  ALIENS = { "sym" => ":dead", "str" => '"dead"', "int" => "9123", "float" => "9.5", "nil" => "nil",
             "ary" => "[:dead]", "obj" => "Object.new" }.freeze
  KINDS = %w[local ivar gvar elem arg return next capture escape raise nop].freeze
  # a method a bare call reaches by its name
  PLAIN = /\A[a-z_][A-Za-z0-9_]*[?!]?\z/
  KEYWORDS = %w[alias and begin break case class def defined? do else elsif end ensure false for if in module
                next nil not or redo rescue retry return self super then true undef unless until when while
                yield].freeze
  # globals Ruby holds to a class of its own
  BUILTIN_GLOBALS = %i[$stdout $stderr $stdin $_ $PROGRAM_NAME $VERBOSE $DEBUG].freeze

  module_function

  # The sites of `source`, in source order of the statement each stands by.
  # A site whose edit does not parse where it stands is left out.
  def sites(source, alien: "sym", guard: GUARD)
    result = Prism.parse(source.b)
    unless result.success?
      error = result.errors.first
      raise ParseError, "line #{error.location.start_line}: #{error.message}"
    end
    found = Finder.new(source.b, ALIENS.fetch(alien), guard).run(result.value)
    found.select { |site| Prism.parse(apply(source, [site])).success? }
  end

  # `source` with `sites` filled in. Two sites at one offset keep the order
  # they were found in.
  def apply(source, sites)
    by = Hash.new { |h, k| h[k] = +"" }
    sites.sort_by(&:id).each { |s| s.inserts.each { |offset, text| by[offset] << text } }
    out = source.b
    by.keys.sort.reverse_each { |offset| out[offset, 0] = by[offset].b }
    out
  end

  # The walk. A scope is the tables of names already written: locals per
  # method or class body, ivars for a class's instances and own_ivars for
  # the class itself, gvars for the program. self_side says which of the two
  # a statement of the body writes ("#" the instances', "." the class's),
  # def_side which a `def` of the body does.
  class Finder
    Scope = Struct.new(:locals, :ivars, :own_ivars, :gvars, :owner, :path, :self_side, :def_side)

    def initialize(source, alien, guard)
      @source = source
      @alien = alien
      @guard = guard
      @sites = []
      @globals = 0
    end

    def run(program)
      walk(program, Scope.new({}, {}, {}, {}, "", nil, "#", "#"), false)
      @sites.sort_by! { |s| [s.inserts.first.first, s.id] }
      @sites.each_with_index { |s, i| s.id = i }
    end

    private

    def utf8(string) = string.to_s.dup.force_encoding(Encoding::UTF_8)

    def text(node) = @source.byteslice(node.location.start_offset...node.location.end_offset)

    def dead(statement) = "#{statement} if #{@guard}; "

    def site(kind, name, node, at, inserts)
      slot = at && [at.location.start_line, at.location.start_column, at.class.name.split("::").last]
      @sites << Site.new(@sites.size, kind, utf8(name), node.location.start_line, slot, inserts)
    end

    # `tight` says the node is a body that takes one expression.
    def walk(node, scope, tight)
      case node
      when Prism::ClassNode, Prism::ModuleNode
        name = text(node.constant_path)
        owner = scope.owner.empty? ? name : "#{scope.owner}::#{name}"
        inner = Scope.new({}, {}, {}, scope.gvars, owner, node.is_a?(Prism::ClassNode) ? name : nil, ".", "#")
        node.compact_child_nodes.each { |c| walk(c, inner, false) }
      when Prism::SingletonClassNode
        inner = Scope.new({}, {}, scope.own_ivars, scope.gvars, scope.owner, nil, ".", ".")
        node.compact_child_nodes.each { |c| walk(c, inner, false) }
      when Prism::DefNode
        side = node.receiver || scope.def_side == "." ? "." : "#"
        ivars = side == "." ? scope.own_ivars : scope.ivars
        inner = Scope.new({}, ivars, ivars, scope.gvars, scope.owner, scope.path, "#", side)
        def_sites(node, inner)
        node.compact_child_nodes.each { |c| walk(c, inner, !node.equal_loc.nil? && c.equal?(node.body)) }
      when Prism::BlockNode, Prism::LambdaNode
        block_sites(node)
        # a local first written in a block is the block's own
        inner = scope.dup.tap { |s| s.locals = s.locals.dup }
        node.compact_child_nodes.each { |c| walk(c, inner, false) }
      when Prism::StatementsNode
        node.body.each_with_index { |st, i| statement_sites(st, node.body[i + 1], scope) } unless tight
        node.body.each { |c| walk(c, scope, false) }
      when Prism::IfNode
        ternary = node.if_keyword_loc.nil?
        node.compact_child_nodes.each { |c| walk(c, scope, ternary) }
      when Prism::ElseNode
        node.compact_child_nodes.each { |c| walk(c, scope, tight) }
      when Prism::EmbeddedStatementsNode
        node.compact_child_nodes.each { |c| walk(c, scope, true) }
      else
        node.compact_child_nodes.each { |c| walk(c, scope, false) }
      end
    end

    # The statements of a method's or a block's body: a body with a rescue
    # or an ensure keeps them one level down.
    def statements(body)
      body = body.statements if body.is_a?(Prism::BeginNode)
      body if body.is_a?(Prism::StatementsNode) && !body.body.empty?
    end

    def statement_sites(st, following, scope)
      case st
      when Prism::LocalVariableWriteNode
        return if scope.locals[st.name]
        scope.locals[st.name] = true
        ahead("local", st, st.name)
        return unless following
        behind("capture", st, following, "#{global} = -> { #{st.name} }")
        behind("escape", st, following, "#{global} = #{st.name}")
        elem(st, following)
      when Prism::InstanceVariableWriteNode
        ivars = scope.self_side == "." ? scope.own_ivars : scope.ivars
        return if ivars[st.name]
        ivars[st.name] = true
        ahead("ivar", st, st.name)
        elem(st, following) if following
      when Prism::GlobalVariableWriteNode
        return if scope.gvars[st.name] || !st.name.match?(/\A\$[a-z_]\w*\z/) || BUILTIN_GLOBALS.include?(st.name)
        scope.gvars[st.name] = true
        ahead("gvar", st, st.name)
        elem(st, following) if following
      end
    end

    def global = "$dead_code_probe_#{@globals += 1}"

    def ahead(kind, write, name)
      site(kind, name, write, write, [[write.location.start_offset, dead("#{name} = #{@alien}")]])
    end

    def behind(kind, write, following, statement)
      site(kind, write.name, following, write, [[following.location.start_offset, dead(statement)]])
    end

    def elem(write, following)
      case write.value
      when Prism::ArrayNode
        behind("elem", write, following, "#{write.name} << #{@alien}") if write.value.opening_loc
      when Prism::HashNode
        behind("elem", write, following, "#{write.name}[#{@alien}] = #{@alien}")
      end
    end

    def block_sites(node)
      stmts = statements(node.body) or return
      first = stmts.body.first
      site("next", "block", first, nil, [[first.location.start_offset, dead("next #{@alien}")]])
    end

    def def_sites(node, scope)
      stmts = statements(node.body) or return
      first = stmts.body.first
      open = first.location.start_offset
      close = nil
      if node.equal_loc
        # `def m = expr` takes the edit and its expression in parentheses;
        # one with a rescue, a heredoc or a second line is left alone
        body = text(stmts)
        return if node.body.is_a?(Prism::BeginNode) || body.include?("<<") || body.include?("\n")
        close = [stmts.location.end_offset, ")"]
      end
      top = lambda do |kind, name, at, statement|
        site(kind, name, first, at, close ? [[open, "(#{dead(statement)}"], close] : [[open, dead(statement)]])
      end
      method = "#{scope.owner}#{scope.def_side unless scope.owner.empty?}#{node.name}"
      top.("nop", method, node, "nil")
      top.("raise", method, node, 'raise "dead"')
      top.("return", method, node, "return #{@alien}") unless node.name.end_with?("=") || node.name == :initialize
      params = node.parameters or return
      list = params.requireds + params.optionals + params.posts
      return unless list.all? { |p| p.respond_to?(:name) && p.name }
      list.each do |param|
        top.("capture", "#{method}(#{param.name})", param, "#{global} = -> { #{param.name} }")
        top.("escape", "#{method}(#{param.name})", param, "#{global} = #{param.name}")
      end
      callee = callee(node, scope) or return
      list.each do |param|
        args = passed_on(params, param) or return
        top.("arg", "#{method}(#{param.name})", param, "#{callee}(#{args.join(", ")})")
      end
    end

    # The arguments of a call that hands every parameter on as it came,
    # but `edited`, which is the alien; nil when a parameter has no name
    # to hand on.
    def passed_on(params, edited)
      pass = ->(p) { p.equal?(edited) ? @alien : p.name.to_s }
      args = (params.requireds + params.optionals).map(&pass)
      if (rest = params.rest)
        return nil unless rest.is_a?(Prism::RestParameterNode) && rest.name
        args << "*#{rest.name}"
      end
      args.concat(params.posts.map(&pass))
      # `def m(if:)` has a keyword no expression can read
      return nil if params.keywords.any? { |k| KEYWORDS.include?(k.name.to_s) }
      args.concat(params.keywords.map { |k| "#{k.name}: #{k.name}" })
      if (kr = params.keyword_rest)
        return nil unless kr.is_a?(Prism::KeywordRestParameterNode) && kr.name
        args << "**#{kr.name}"
      end
      if (block = params.block)
        return nil unless block.name
        args << "&#{block.name}"
      end
      args
    end

    # How the method is called from its own body: by its name, or, for the
    # initialize of a class, as `new` on the class.
    def callee(node, scope)
      name = node.name.to_s
      return "#{scope.path}.new" if name == "initialize" && scope.path && scope.def_side == "#"
      name if name.match?(PLAIN) && !KEYWORDS.include?(name) && name != "initialize"
    end
  end
end
