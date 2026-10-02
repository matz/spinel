# Source permutation for the order probe (see tools/order_probe.rb).
#
#   runs = OrderPermute.runs(source)
#   OrderPermute.permute(source, runs, "reverse").each { |p| p.text; p.line_map }
#   OrderPermute.control(source, runs, "shift")
#   OrderPermute.facts(source)
#
# The probe asks whether the types Spinel settles on depend on the order a
# program's definitions are written in. This file is the half that needs only
# Prism: it finds the definitions that may trade places and writes the
# program again with them moved, with a map from each new line to the line it
# came from, so two type dumps can be compared node for node.
#
# A run is two or more consecutive statements of one body, each a movable
# definition: a `def`, or one of the six visibility calls of WRAPPERS around
# a single `def`. Bodies are those of the program, a class, a module and
# `class << x`, where the class is itself a statement of such a body. A run
# does not reach into a method body, a block, or a class whose value is
# used: the value of `x = class A ... end` is its last def's name, which a
# move changes. A bare `private`, an `attr_reader`, any statement that is
# not a movable definition, ends the run, so nothing moves across it.
#
# A run is found and still rejected, with a reason the probe counts, when a
# move could change what the program does or parses as: two units defining
# one name (the later wins), a unit sharing a line with other code, a
# `method_added` hook (it sees the defs in order), a visibility call beside a
# def of that call's own name (which method the call reaches changes), or a
# comment Ruby or Spinel acts on between two units. These rules make a wrong
# permutation rare, not impossible: the probe still runs both orders under
# CRuby before it believes a difference.
#
# Units move as whole lines. A unit owns the lines from the one after the
# unit before it to its own last line, so a comment above a def goes where
# the def goes; the first unit of a run starts at its own first line, and
# what stands above it stays. The last line covers a heredoc the unit opens:
# Prism ends `def b = <<~X` on the def's line, and a move that left the body
# behind would hand it the next def as text. Every line is written with a
# newline, or `end` and a following `def` at the end of a file without one
# join into `enddef`, which is valid Ruby that defines nothing.
#
# Sources are binary Strings and every offset is in bytes. Prism's columns
# are bytes, and under the C locale a String read as text is sliced by
# characters, which differ from bytes on any line with a multibyte literal.
# Names are returned as UTF-8, as the type dump's are.

require "digest"
require "prism"

module OrderPermute
  # Prism reported errors for the source.
  class ParseError < StandardError; end

  # One movable definition of a run. Lines are 1-based lines of the source:
  # first_line..last_line are the whole lines the unit owns, def_line is the
  # line of the `def` keyword. receiver is "self" (or the receiver's source)
  # for `def self.x`, nil otherwise. kind is :def; nothing below the scan
  # reads the node again, so a unit of another kind needs only these fields.
  Unit = Struct.new(:name, :receiver, :kind, :first_line, :last_line, :def_line)

  # index counts every run of the file from 0 in source order, rejected ones
  # included, so a run keeps its number whatever the rules reject. owner is
  # the nesting path: "" at top level, "Rng", "Outer::Inner", and the body of
  # `class << self` in Rng as "Rng.singleton". reject is nil or the reason.
  Run = Struct.new(:index, :owner, :units, :reject)

  # text is the new source. line_map[new_line] is the old line, nil for a
  # line a control inserted; index 0 is unused. orders is { run.index =>
  # [unit positions in their new order] } for the runs that moved, so two
  # Permuted of one source are equal exactly when their texts are.
  Permuted = Struct.new(:text, :line_map, :orders)

  WRAPPERS = %w[private public protected module_function private_class_method public_class_method].freeze
  HOOKS = %w[method_added singleton_method_added].freeze
  # The comments Ruby acts on where they stand, and those it and Spinel read
  # at the top of a file. `# note: slow` has the same shape and is not one.
  DIRECTIVE = /\A\s*#.*\b(?:shareable[-_]constant[-_]value|frozen[-_]string[-_]literal|warn[-_]indent|(?:en)?coding)\s*:/i
  # Ways a method runs the block it was given as a parameter.
  BLOCK_CALLS = %i[call yield []].freeze
  STRATEGIES = %w[reverse rotate shuffle swap].freeze
  CONTROLS = %w[null shift ids].freeze
  # What the controls insert. The comment has no colon, so nothing reads it as a directive.
  SHIFT_LINE = "# order probe line shift control\n"
  ID_LINE = "nil\n"

  module_function

  # Every run of the source, in source order.
  def runs(source)
    found = []
    scan(parse(source).statements, "", source.b.lines, found)
    found.each_with_index { |run, i| run.index = i }
  end

  # What the type comparison needs to know about the source, as plain data:
  # every def of the file, nested ones too, and every class, module and
  # singleton-class body, each with its owner path and lines. A def is
  # singleton with a receiver or inside `class << x`; its end_line is the
  # def node's own, not that of a heredoc below it.
  def facts(source)
    out = { defs: [], classes: [] }
    gather(parse(source), "", false, out)
    out
  end

  # The general writer: the source with each run named in orders written in
  # that order of its units, and every other line where it was.
  def apply(source, runs, orders)
    lines = source.b.lines
    seq = (1..lines.size).to_a
    moved = orders.map do |index, order|
      run = runs.find { |r| r.index == index }
      raise ArgumentError, "no run #{index} to move" unless run && run.reject.nil?
      raise ArgumentError, "run #{index}: #{order.inspect} is not an order of its units" unless order.sort == file_order(run)
      [run, order]
    end
    moved.reject! { |run, order| order == file_order(run) }
    # Smaller spans first: a run nested in a unit of another is written
    # before that unit's lines are lifted, and so travels with it.
    moved.sort_by { |run, _| run.units.last.last_line - run.units.first.first_line }.each do |run, order|
      span = (run.units.first.first_line - 1)..(run.units.last.last_line - 1)
      seq[span] = order.flat_map { |k| seq[(run.units[k].first_line - 1)..(run.units[k].last_line - 1)] }
    end
    emit(lines, seq, moved.to_h { |run, order| [run.index, order] }.sort.to_h)
  end

  # The permuted copies a strategy gives. reverse, rotate and shuffle move
  # every accepted run at once and give one copy; swap gives one copy per
  # pair of units of each accepted run, with that pair exchanged and all
  # else in file order. A source with nothing to move gives none.
  # limit is the most copies wanted, the first the strategy gives.
  def permute(source, runs, strategy, seed: 0, path: "", limit: nil)
    all = orders(runs, strategy, seed: seed, path: path)
    (limit ? all.first(limit) : all).map { |order| apply(source, runs, order) }
  end

  # What permute hands to apply, one Hash per copy, as an Enumerator. One
  # corpus program has a run of 1,030 units, which swap turns into 529,935
  # copies of a 22 KB file: a caller takes as many as it will compile, and
  # the pairs come neighbours first, of every run, the smallest moves ahead
  # of the rest.
  def orders(runs, strategy, seed: 0, path: "")
    raise ArgumentError, "unknown strategy #{strategy}" unless STRATEGIES.include?(strategy)
    live = runs.reject(&:reject)
    whole = { "reverse" => ->(run) { file_order(run).reverse }, "rotate" => ->(run) { file_order(run).rotate },
              "shuffle" => ->(run) { shuffled(run, seed, path) } }[strategy]
    Enumerator.new do |out|
      out << live.to_h { |run| [run.index, whole.(run)] } if whole && !live.empty?
      next if whole
      # every run's neighbours, then every run's pairs two apart, and so on:
      # the first few are the nearest pairs of the program, not of its first run
      (1...live.map { |run| run.units.size }.max.to_i).each do |apart|
        live.each do |run|
          (0...(run.units.size - apart)).each do |i|
            order = file_order(run)
            order[i], order[i + apart] = order[i + apart], order[i]
            out << { run.index => order }
          end
        end
      end
    end
  end

  def file_order(run) = (0...run.units.size).to_a

  # The copies that move nothing, which the probe must find no difference in.
  # null is the source through the writer. shift puts a comment line ahead of
  # the second unit of every accepted run, so the lines below it move as a
  # permutation moves them. ids puts a `nil` statement ahead of the first
  # statement that is no `require`, below the comments that open the file (a
  # shebang and the magic comments must stay on top), so every later node
  # gets another id.
  def control(source, runs, which)
    lines = source.b.lines
    seq = (1..lines.size).to_a
    case which
    when "null"
    when "shift"
      runs.reject(&:reject).map { |run| run.units[1].first_line }.sort.reverse_each { |at| seq.insert(at - 1, SHIFT_LINE) }
    when "ids"
      # Below the requires that open the program as well: spinel puts a
      # required file's text where its `require` stands, and the lines above
      # one are off by less than those below it.
      first = parse(source).statements.body.drop_while { |stmt| requires?(stmt) }.first
      seq.insert(first ? first.location.start_line - 1 : seq.size, ID_LINE)
    else
      raise ArgumentError, "unknown control #{which}"
    end
    emit(lines, seq, {})
  end

  # Below: what the functions above are built from.

  def requires?(stmt)
    stmt.is_a?(Prism::CallNode) && stmt.receiver.nil? && %i[require require_relative].include?(stmt.name)
  end

  def parse(source)
    result = Prism.parse(source.b)
    return result.value if result.errors.empty?
    error = result.errors.first
    raise ParseError, "line #{error.location.start_line}: #{error.message}"
  end

  def utf8(string) = string.dup.force_encoding(Encoding::UTF_8)

  # The def a statement defines, when the statement is a movable definition.
  # A wrapper is one only in its plain form: `memoize def x` runs code of the
  # program's own at definition time, and `private def x, :y` or a block
  # argument is some other call.
  def definition(stmt)
    return stmt if stmt.is_a?(Prism::DefNode)
    return nil unless stmt.is_a?(Prism::CallNode) && stmt.receiver.nil? && stmt.block.nil?
    return nil unless WRAPPERS.include?(stmt.name.to_s)
    args = stmt.arguments ? stmt.arguments.arguments : []
    args.size == 1 && args[0].is_a?(Prism::DefNode) ? args[0] : nil
  end

  # Collects the runs of one body, then of each class, module or singleton
  # class that is a statement of it. A body with a `rescue` is a BeginNode
  # and is left alone, as is everything reached through any other statement.
  def scan(body, owner, lines, found)
    return unless body.is_a?(Prism::StatementsNode)
    members = []
    close = lambda do
      found << build_run(owner, members, lines) if members.size > 1
      members = []
    end
    body.body.each do |stmt|
      if (defn = definition(stmt))
        members << [stmt, defn]
        next
      end
      close.()
      scan(stmt.body, scope(owner, stmt), lines, found) if scope?(stmt)
    end
    close.()
  end

  def scope?(node)
    node.is_a?(Prism::ClassNode) || node.is_a?(Prism::ModuleNode) || node.is_a?(Prism::SingletonClassNode)
  end

  def scope(owner, node)
    return "#{owner}.singleton" if node.is_a?(Prism::SingletonClassNode)
    name = utf8(node.constant_path.location.slice)
    name.start_with?("::") ? name[2..] : [owner, name].reject(&:empty?).join("::")
  end

  def build_run(owner, members, lines)
    units = []
    whole = true
    members.each do |stmt, defn|
      start = stmt.location
      last = extent(stmt)[1]
      # A unit owns whole lines when it starts below the unit before it and
      # is alone on its lines.
      whole &&= (units.empty? || start.start_line > units.last.last_line) && owns_lines?(stmt, lines)
      # In a run rejected for sharing lines a unit still starts no later than its statement.
      first = units.empty? ? start.start_line : [units.last.last_line + 1, start.start_line].min
      units << Unit.new(utf8(defn.name.to_s), defn.receiver && utf8(defn.receiver.location.slice), :def,
                        first, last, defn.location.start_line)
    end
    Run.new(nil, owner, units, whole ? reject_reason(members, units, lines) : "partial-line")
  end

  # Whether a statement is alone on its lines, but for indentation ahead of
  # it and a comment after it.
  def owns_lines?(stmt, lines)
    (stop_line, stop_column), = extent(stmt)
    start = stmt.location
    after = lines[stop_line - 1].byteslice(stop_column..).to_s.strip
    lines[start.start_line - 1].byteslice(0, start.start_column).strip.empty? && (after.empty? || after.start_with?("#"))
  end

  # Where a node ends: the [line, column] past its last token, and the last
  # line its text reaches. They differ for a node that opens a heredoc, whose
  # body and terminator lie below the opening token: code after the node is
  # looked for at the first, and the unit owns up to the second. A closing
  # location is counted itself because Prism 0.19 ends `m(&b)` before its `)`.
  def extent(node)
    stop = [node.location.end_line, node.location.end_column]
    last = stop[0]
    closing = node.respond_to?(:closing_loc) && node.closing_loc
    heredoc = closing && node.respond_to?(:opening_loc) && node.opening_loc && node.opening_loc.slice.start_with?("<<")
    return [stop, closing.start_line] if heredoc
    stop = [stop, [closing.end_line, closing.end_column]].max if closing
    node.compact_child_nodes.each do |child|
      child_stop, child_last = extent(child)
      stop = [stop, child_stop].max
      last = [last, child_last].max
    end
    [stop, [last, stop[0]].max]
  end

  def reject_reason(members, units, lines)
    return "duplicate" if units.map { |u| [u.receiver, u.name] }.uniq.size < units.size
    return "hook" if units.any? { |u| HOOKS.include?(u.name) }
    wrappers = members.reject { |stmt, defn| stmt.equal?(defn) }.map { |stmt, _| stmt.name.to_s }
    return "wrapper-name" if units.any? { |u| wrappers.include?(u.name) }
    # Only the lines between two units travel; what is above the first stays.
    gaps = units.zip(members).drop(1).flat_map { |u, (stmt, _)| lines[(u.first_line - 1)...(stmt.location.start_line - 1)] }
    return "directive" if gaps.any? { |line| line.match?(DIRECTIVE) }
    nil
  end

  # A unit order for the run that depends on nothing but the seed, the
  # program's path and the run's index: not on the files read before this
  # one, nor on the process's own generator. It is never the file order, so
  # a shuffle always moves something.
  def shuffled(run, seed, path)
    rng = Random.new(Digest::SHA256.hexdigest([seed, path, run.index].join("\0")).to_i(16))
    loop do
      order = file_order(run).shuffle(random: rng)
      return order unless order == file_order(run)
    end
  end

  # seq is the new file: an old line number, or the text of an inserted line.
  def emit(lines, seq, orders)
    text = "".b
    seq.each do |from|
      line = from.is_a?(Integer) ? lines[from - 1] : from
      text << line
      text << "\n" unless line.end_with?("\n")
    end
    Permuted.new(text, [nil] + seq.map { |from| from.is_a?(Integer) ? from : nil }, orders)
  end

  def gather(node, owner, in_singleton, out)
    if scope?(node)
      owner = scope(owner, node)
      in_singleton = node.is_a?(Prism::SingletonClassNode)
      out[:classes] << { name: owner, line: node.location.start_line, end_line: node.location.end_line }
    elsif node.is_a?(Prism::DefNode)
      block = node.parameters && node.parameters.block && node.parameters.block.name
      out[:defs] << { name: utf8(node.name.to_s), owner: owner, singleton: in_singleton || !node.receiver.nil?,
                      line: node.location.start_line, end_line: node.location.end_line,
                      yields: yields?(node.body, block), params: params(node.parameters) }
    end
    node.compact_child_nodes.each { |child| gather(child, owner, in_singleton, out) }
  end

  # Whether a method body yields or calls the block parameter named block.
  # A def nested in the body is another method and is not looked into.
  def yields?(node, block)
    return false if node.nil? || node.is_a?(Prism::DefNode)
    return true if node.is_a?(Prism::YieldNode)
    return true if block && node.is_a?(Prism::CallNode) && BLOCK_CALLS.include?(node.name) &&
                   node.receiver.is_a?(Prism::LocalVariableReadNode) && node.receiver.name == block
    node.compact_child_nodes.any? { |child| yields?(child, block) }
  end

  # Parameter names in the order they are written. A destructured parameter
  # gives the names inside it, and one with no name gives its sigil.
  def params(node)
    return [] unless node
    [*node.requireds, *node.optionals, node.rest, *node.posts, *node.keywords, node.keyword_rest, node.block]
      .compact.flat_map { |param| param_names(param) }
  end

  def param_names(param)
    case param
    when Prism::MultiTargetNode then [*param.lefts, param.rest, *param.rights].compact.flat_map { |inner| param_names(inner) }
    when Prism::SplatNode then param.expression ? param_names(param.expression) : ["*"]
    when Prism::ForwardingParameterNode then ["..."]
    else
      return [] unless param.respond_to?(:name)
      [utf8(param.name ? param.name.to_s : param.operator)]
    end
  end
end
