# The type-dump half of the order probe (see tools/order_probe.rb): two
# `spinel --emit-types` documents of one program, the second compiled from a
# copy with sibling definitions written in another order, are compared slot
# by slot.
#
#   result = OrderTypes.compare(doc_a, doc_b, file_a: "a/x.rb", file_b: "b/x.rb",
#                               facts: facts, runs: runs, line_map: line_map)
#
# The module reads nothing but JSON. What it has to know of the source comes
# as plain data from tools/order_permute.rb: every def and class with its
# lines (facts), the runs of definitions that moved (runs), and for each line
# of the permuted source the line of the file-order source it came from
# (line_map, nil for a line a control inserted). A Hash or a Struct with the
# same members does for each.
#
# Four things in a type dump change between two compiles without any type
# changing, and each is taken out before anything is compared:
#
# - The compiler puts text ahead of the program (a line for each builtin file
#   it splices in, or a whole builtin file), so every line of the main file is
#   off by a constant. The constant is measured, per compile, as the shift
#   under which every def of the source has a DefNode record of its name on
#   its line. A program where no shift does that is reported unalignable, not
#   compared. Records that fall before line 1 are the pasted file's.
# - A record's position is in the order its compile read. Lines of the
#   permuted compile are mapped back to file-order lines. A record that
#   covers more than one of the definitions that moved (the body that holds
#   them) starts and ends in different definitions in each order, so it is
#   keyed by the lines it covers, without columns, and is never a slot of its
#   own: its type is the type of whichever definition is last. A difference
#   there is listed as detail when both orders have the record.
# - Names the compiler makes up carry node ids (`x__bp28`, `__cap_38_s`,
#   `__enum_find__1931`, `obj_StructAnon_1220`), and ids move when definitions
#   do. They are removed from names and from type text.
# - A cloned or inlined body writes a record of its own at the span it was
#   copied from, so one key holds several records, in an order that follows
#   the definitions. The value compared under a key is the set of its types.
#
# A slot that differs is given one class, the first of these that applies:
#
#   instantiation   it is inside a method that yields, and that method's
#                   parameter types are the same in both orders: a yielding
#                   method is inlined at each call site and the dump shows one
#                   of the copies (docs/limitations.md)
#   shape           the slot has a record in one order only, or a signature
#                   has another number of parameters, or other parameters
#                   where the compiler makes them up (`def m(...)`)
#   representation  the RBS is the same and the internal tag is not
#   precision       one side is the other with parts replaced by `untyped`;
#                   `loser` names the side that has the untyped
#   disagreement    two types neither of which refines the other
#
# Precision is decided on the structure of the RBS, not on the tag: an
# `int_array` against a `poly_array` is Array[Integer] against Array[untyped].
#
# Slots are grouped under roots, named without line numbers so a root is
# still the same root after the program is cut down: `Rng#nxt` (a method: its
# return, and every differing node in its body that has no root of its own),
# `Rng#nxt(x)` (a parameter), `Rng@s0` (an instance variable, wherever it is
# read or written), `Rng.build` (a singleton method), `#name` (a top-level
# method), a top-level local by its name, and `(top level)` or `Rng (body)`
# for what is left outside any method. A record of another file (a builtin,
# a package) is compared too and belongs to no root.

require "json"

module OrderTypes
  # The type-tier classes, most severe first.
  SEVERITY = %w[disagreement precision shape representation instantiation].freeze

  # status is :ok or :unalignable (with reason). detail lists what differs
  # and is no slot: enclosing spans, and records with no end position, which
  # are compared by (kind, name) as the types each side has more of.
  Result = Struct.new(:status, :reason, :shift_a, :shift_b, :slots, :roots, :detail, keyword_init: true)
  # Lines are file-order lines of the main file; file is nil there and the
  # path from builtins/ or packages/ on for any other. types_* are the
  # internal tags of each side and rbs_* the RBS, sorted; for a slot made
  # from a signature rbs_* is that parameter's or the return's type and
  # types_* is empty. loser is :a or :b for precision, nil otherwise.
  Slot = Struct.new(:file, :line, :col, :end_line, :kind, :name, :types_a, :types_b, :rbs_a, :rbs_b,
                    :klass, :loser, :root, keyword_init: true)
  # klass is the most severe class among slots, pair the [file-order,
  # permuted] RBS of the slot that has it (nil for a side with no record).
  Root = Struct.new(:name, :slots, :klass, :pair, keyword_init: true)
  Detail = Struct.new(:what, :line, :end_line, :kind, :name, :types_a, :types_b, keyword_init: true)

  # Ids appended to a name the user wrote, removed whole: a block's parameter
  # is `i` at one call site and `i__bp98` at another, and which site gets
  # which follows the order. Formats as in src/: %s__bp%d, %s__bp%d_%d,
  # _%d__b%d, %s__sg_%d, %s__sg_%d_%d, %s__dup%d, %s__redef%d, %s__main%d,
  # %s#__cond%d.
  NAME_ID = /#?__(?:bp|sg_|dup|redef|main|cond|b)\d+(?:_\d+)?/
  # The same class ids as they come out in RBS, where `_` reads as `::`.
  RBS_ID = /::::(?:sg::|dup|redef|main)\d+(?:::\d+)?/
  ANON_ID = /(StructAnon|SpinelAnonClass)(?:_|::)?\d+/
  # A made-up name with the id ahead of a user's name: __cap_38_s,
  # $__dmcap286_n. What follows the id is kept as written.
  HEAD_ID = /\A(\$?__[a-z]+)_?\d+_(?=\D)/
  # The key of the program's top-level body (see table).
  PROGRAM = [nil, nil, nil, nil, nil, "StatementsNode", nil].freeze
  RBS_TOKEN = /->|[\[\](),|?^]|[^\s\[\](),|?^]+/

  class RbsError < StandardError; end

  module_function

  # A dump as a document. It is read as bytes, so the locale decides
  # nothing, and a name holding bytes that are not UTF-8 still parses.
  def read(path) = JSON.parse(File.binread(path).force_encoding("UTF-8").scrub)

  # A name with the compiler's ids removed. Digits the user wrote stay:
  # only a name that starts with `__` (none of the user's do in practice)
  # loses a numeric tail, `__enum_find__1931`, `__fwd_7_2`, `__blk_kwrest2597`.
  def norm_name(name)
    return name unless name && name.match?(/\d/)
    name = name.gsub(NAME_ID, "").gsub(ANON_ID, "\\1")
    return name unless name.start_with?("__", "$__")
    return name if name.sub!(HEAD_ID, "\\1_")
    # __sp_default_<method>_<param>_<id>: the method and the parameter are the user's.
    name.sub(name.start_with?("__sp_default_") ? /_\d+\z/ : /\d*(?:_+\d+)*\z/, "")
  end

  # Type, RBS or refusal text with the same ids removed.
  def norm_text(text)
    return text.to_s unless text && text.match?(/\d/)
    text.gsub(NAME_ID, "").gsub(RBS_ID, "").gsub(ANON_ID, "\\1").gsub(/\bnode \d+/, "node N")
  end

  # --- RBS ------------------------------------------------------------------

  # A type as [head, parts]: a name with its arguments (["Array",
  # [["Integer", []]]]), "|" for a union, "?" for an optional, "[]" for a
  # tuple, "->" for a method or proc type (the parameters, then the return)
  # and "^" around a proc. Text that does not parse is one opaque name.
  def parse_rbs(text)
    tokens = text.scan(RBS_TOKEN)
    type = rbs_union(tokens)
    tokens.empty? ? type : [text, []]
  rescue RbsError
    [text, []]
  end

  def rbs_union(tokens)
    parts = [rbs_postfix(tokens)]
    parts << rbs_postfix(tokens) while tokens.first == "|" && tokens.shift
    parts.size == 1 ? parts[0] : ["|", parts]
  end

  def rbs_postfix(tokens)
    type = rbs_atom(tokens)
    type = ["?", [type]] while tokens.first == "?" && tokens.shift
    type
  end

  def rbs_atom(tokens)
    case token = tokens.shift
    when "^" then ["^", [rbs_atom(tokens)]]
    when "[" then ["[]", rbs_list(tokens, "]")]
    when "("
      parts = rbs_list(tokens, ")")
      return ["->", parts << rbs_union(tokens)] if tokens.first == "->" && tokens.shift
      parts.size == 1 ? parts[0] : raise(RbsError)
    when /\A\w/ then [token, tokens.first == "[" && tokens.shift ? rbs_list(tokens, "]") : []]
    else raise RbsError
    end
  end

  def rbs_list(tokens, close)
    parts = []
    until tokens.first == close
      parts << rbs_union(tokens)
      break unless tokens.first == "," && tokens.shift
    end
    tokens.shift == close ? parts : raise(RbsError)
  end

  def rbs_text(type)
    head, parts = type
    inner = parts.map { |part| rbs_text(part) }
    case head
    when "->" then "(#{inner[0..-2].join(", ")}) -> #{inner[-1]}"
    when "|" then inner.join(" | ")
    when "?" then "#{inner[0]}?"
    when "^" then "^#{inner[0]}"
    when "[]" then "[#{inner.join(", ")}]"
    else parts.empty? ? head : "#{head}[#{inner.join(", ")}]"
    end
  end

  # Whether b is a with one or more parts replaced by `untyped`.
  def refines?(a, b) = a != b && widening?(a, b)

  def widening?(a, b)
    return true if b == ["untyped", []]
    a[0] == b[0] && a[1].size == b[1].size && a[1].zip(b[1]).all? { |x, y| widening?(x, y) }
  end

  # The class of two differing sets of RBS text, and the side that lost.
  # A side keeps its precision when each type only it has refines one of
  # the other side's, and each type only the other has is refined by one
  # of its own. A clone boxed in one order only is thus precision as well.
  def classify(rbs_a, rbs_b)
    return ["shape", nil] if rbs_a.empty? || rbs_b.empty?
    return ["representation", nil] if rbs_a.sort == rbs_b.sort
    a = rbs_a.map { |text| parse_rbs(text) }
    b = rbs_b.map { |text| parse_rbs(text) }
    keeps = lambda do |mine, other|
      (mine - other).all? { |x| other.any? { |y| refines?(x, y) } } &&
        (other - mine).all? { |y| mine.any? { |x| refines?(x, y) } }
    end
    a_keeps = keeps.(a, b)
    a_keeps == keeps.(b, a) ? ["disagreement", nil] : ["precision", a_keeps ? :b : :a]
  end

  # --- alignment ------------------------------------------------------------

  # The lines of the file-order source: the innermost def and class each is
  # in, and the unit of a run it belongs to, as [run index, first line,
  # last line].
  Layout = Struct.new(:def_at, :class_at, :unit_at, :defs)

  def layout_of(facts, runs)
    innermost = lambda do |spans|
      at = []
      spans.sort_by { |s| s[:line] - s[:end_line] }.each { |s| (s[:line]..s[:end_line]).each { |line| at[line] = s } }
      at
    end
    unit_at = []
    units = runs.flat_map { |run| (run[:units] || []).map { |unit| [run[:index], unit[:first_line], unit[:last_line]] } }
    units.sort_by { |_, first, last| first - last }.each { |unit| (unit[1]..unit[2]).each { |line| unit_at[line] = unit } }
    Layout.new(innermost.(facts[:defs]), innermost.(facts[:classes]), unit_at,
               facts[:defs].to_h { |d| [[norm_name(d[:name]), d[:line]], d] })
  end

  # The shift of one compile and nil, or nil and why there is none. wanted
  # is [name, line] of every def as that compile's source has it, the name
  # normalised as a record's is (a redefined `g` is recorded `g__redef1`).
  # Defs the compiler made up have a record and no def, and decide nothing.
  def shift(doc, file, wanted)
    at = Hash.new { |h, k| h[k] = [] }
    doc["types"].each { |r| at[norm_name(r["name"])] << r["line"] if r["kind"] == "DefNode" && r["file"] == file }
    return [nil, "the source has no def to measure the line shift from"] if wanted.empty?
    lost = wanted.find { |_, line| line.nil? }
    return [nil, "the line map has no line for def `#{lost[0]}`"] if lost
    fits = Hash.new(0)
    wanted.each { |name, line| at[name].uniq.each { |recorded| fits[recorded - line] += 1 } }
    need = wanted.tally
    candidates = fits.keys.sort_by { |s| [-fits[s], s.abs] }
    found = candidates.find { |s| need.all? { |(name, line), count| at[name].count(line + s) >= count } }
    return [found, nil] if found
    best = candidates.first
    name, line = wanted.find { |n, l| best.nil? || !at[n].include?(l + best) }
    [nil, "no line shift gives every def a DefNode record: `#{name}` (line #{line}) has none" +
          (best ? " at shift #{best}, which fits #{fits[best]} of #{wanted.size}" : "")]
  end

  # The file-order lines a record covers, first and last, and whether they
  # lie in more than one unit of a run. map is the permuted compile's line
  # map, nil for the file-order compile. Lines a control inserted map to
  # nothing: a record wholly on them has no span. An enclosing span is
  # widened to the edges of the units it starts and ends in, since a unit's
  # trailing heredoc or comment lines are inside it in one order only.
  def span(low, high, map, layout)
    first = last = nil
    seen = {}
    (low..high).each do |line|
      line = map[line] if map
      next unless line
      first = line if first.nil? || line < first
      last = line if last.nil? || line > last
      unit = layout.unit_at[line]
      (seen[unit[0]] ||= {})[unit[1]] = true if unit
    end
    return [first, last, false] unless seen.any? { |_, units| units.size > 1 }
    [(layout.unit_at[first] || [nil, first])[1], (layout.unit_at[last] || [nil, nil, last])[2], true]
  end

  # One compile's records: keyed is { key => [[type, rbs, signature], ...] }
  # for those with a span, loose a count of each [kind, name, type] without
  # one, renamed the keys whose name the compiler changed (a block's
  # parameter, mostly). A key is [file, line, col, end line, end col, kind,
  # name], with file nil for the main file and both columns nil for an
  # enclosing span; another file's record keeps the lines it has. The first
  # record is the program's top-level body: it starts where the first
  # statement does, in a file spliced ahead of the program or on a line a
  # control inserted, and ends on the program's last line, so no position
  # keys it.
  Table = Struct.new(:keyed, :loose, :renamed)

  def table(doc, file, shift, map, layout)
    dir = File.dirname(file) + "/"
    out = Table.new(Hash.new { |h, k| h[k] = [] }, Hash.new(0), {})
    doc["types"].each_with_index do |r, i|
      main = r["file"] == file
      line = main ? r["line"] - shift : r["line"]
      name = norm_name(r["name"])
      value = [norm_text(r["type"]), norm_text(r["rbs"]), norm_text(r["signature"])]
      if i == 0 && r["kind"] == "StatementsNode"
        out.keyed[PROGRAM] << value
      elsif main && line < 1
        next
      elsif r["end_line"].nil?
        out.loose[[r["kind"], name, value[0]]] += 1
      elsif main
        first, last, enclosing = span(line, r["end_line"] - shift, map, layout)
        next unless first
        key = [nil, first, enclosing ? nil : r["col"], last, enclosing ? nil : r["end_col"], r["kind"], name]
        out.keyed[key] << value
        out.renamed[key] = true if name != r["name"]
      else
        path = r["file"][%r{(?:builtins|packages)/(?!.*(?:builtins|packages)/).*}] || r["file"].delete_prefix(dir)
        out.keyed[[path, line, r["col"], r["end_line"], r["end_col"], r["kind"], name]] << value
      end
    end
    out
  end

  # --- comparison -----------------------------------------------------------

  def compare(doc_a, doc_b, file_a:, file_b:, facts:, runs:, line_map:)
    result = Result.new(status: :unalignable, slots: [], roots: {}, detail: [])
    layout = layout_of(facts, runs)
    permuted_line = []
    line_map.each_with_index { |old, new| permuted_line[old] = new if old && new > 0 }
    wanted = facts[:defs].map { |d| [norm_name(d[:name]), d[:line]] }
    result.shift_a, why = shift(doc_a, file_a, wanted)
    return result.tap { result.reason = "file-order compile: #{why}" } unless result.shift_a
    result.shift_b, why = shift(doc_b, file_b, wanted.map { |name, line| [name, permuted_line[line]] })
    return result.tap { result.reason = "permuted compile: #{why}" } unless result.shift_b

    a = table(doc_a, file_a, result.shift_a, nil, layout)
    b = table(doc_b, file_b, result.shift_b, line_map, layout)
    # Each order's signatures of a def, and whether the parameter types in
    # them are the same in both orders.
    signatures = [a, b].map do |side|
      side.keyed.each_with_object(Hash.new { |h, k| h[k] = [] }) do |(key, values), by_def|
        by_def[[key[6], key[1]]].concat(values.map(&:last)) if key[0].nil? && key[5] == "DefNode"
      end
    end
    same_params = Hash.new do |h, d|
      params = signatures.map do |by_def|
        by_def[[norm_name(d[:name]), d[:line]]].uniq.map { |text| parse_rbs(text)[1][0..-2] }.sort_by(&:inspect)
      end
      h[d] = !params[0].empty? && params[0] == params[1]
    end

    (a.keyed.keys | b.keyed.keys).each do |key|
      mine = a.keyed.fetch(key, []).uniq.sort
      theirs = b.keyed.fetch(key, []).uniq.sort
      next if mine == theirs
      at = { file: key[0], line: key[1], col: key[2], end_line: key[3], kind: key[5], name: key[6] }
      tags = [mine, theirs].map { |values| values.map(&:first).uniq }
      if at[:file].nil? && at[:col].nil?
        next if mine.empty? || theirs.empty?
        result.detail << Detail.new(what: "enclosing span", types_a: tags[0], types_b: tags[1],
                                    **at.slice(:line, :end_line, :kind, :name))
        next
      end
      d = at[:file].nil? && at[:kind] == "DefNode" ? layout.defs[[at[:name], at[:line]]] : nil
      if d && mine.map(&:last) != theirs.map(&:last)
        sigs = [mine, theirs].map { |values| values.map(&:last).uniq }
        result.slots.concat(signature_slots(at, d, sigs[0], sigs[1], same_params[d]))
        next
      end
      # A def the source does not have is compared by its signature.
      rbs = [mine, theirs].map { |values| values.map { |v| v[2].empty? ? v[1] : v[2] }.uniq }
      klass, loser = classify(*rbs)
      inside = at[:file].nil? ? layout.def_at[at[:line]] : nil
      klass, loser = "instantiation", nil if inside && inside[:yields] && same_params[inside]
      root = at[:file] ? nil : root_name(at, layout, a.renamed[key] || b.renamed[key])
      result.slots << Slot.new(types_a: tags[0], types_b: tags[1], rbs_a: rbs[0], rbs_b: rbs[1],
                               klass: klass, loser: loser, root: root, **at)
    end

    # Records with no end position, by [kind, name]: the types each order
    # has more of than the other.
    (a.loose.keys | b.loose.keys).group_by { |key| key[0, 2] }.sort_by { |k, _| k.map(&:to_s) }.each do |(kind, name), keys|
      more = ->(mine, theirs) { keys.flat_map { |k| [k[2]] * [mine[k] - theirs[k], 0].max }.sort }
      only_a, only_b = more.(a.loose, b.loose), more.(b.loose, a.loose)
      next if only_a.empty? && only_b.empty?
      result.detail << Detail.new(what: "no end position", kind: kind, name: name, types_a: only_a, types_b: only_b)
    end

    result.slots.sort_by! { |s| [s.file.to_s, s.line, s.col, s.kind, s.name.to_s, s.root.to_s] }
    result.slots.group_by(&:root).each do |name, slots|
      next unless name
      top = slots.min_by { |s| [SEVERITY.index(s.klass), s.kind == "DefNode" ? 0 : 1] }
      pair = [top.rbs_a, top.rbs_b].map { |rbs| rbs.empty? ? nil : rbs.join(", ") }
      result.roots[name] = Root.new(name: name, slots: slots, klass: top.klass, pair: pair)
    end
    result.status = :ok
    result
  end

  # The slots of a def whose signatures differ: one per parameter that
  # differs and one for the return, when each order has one signature of
  # the same length; one for the def otherwise. A yielding def whose
  # parameters agree shows one of its copies, so its return is no finding.
  # The parameters of `def m(...)` are the compiler's: it makes a rest
  # array or concrete parameters from the call sites, and which it makes
  # is the def's shape, not a type that widened.
  def signature_slots(at, d, sigs_a, sigs_b, same_params)
    base = def_root(d)
    slot = lambda do |rbs_a, rbs_b, root, klass = nil|
      klass, loser = klass ? [klass, nil] : classify(rbs_a, rbs_b)
      Slot.new(types_a: [], types_b: [], rbs_a: rbs_a, rbs_b: rbs_b, klass: klass, loser: loser, root: root, **at)
    end
    copy = d[:yields] && same_params ? "instantiation" : nil
    a, b = [sigs_a, sigs_b].map { |sigs| sigs.size == 1 ? parse_rbs(sigs[0]) : nil }
    return [slot.(sigs_a, sigs_b, base, copy)] unless a && b && a[0] == "->" && b[0] == "->"
    made_up = d[:params].include?("...") && a[1][0..-2] != b[1][0..-2]
    return [slot.(sigs_a, sigs_b, base, "shape")] if a[1].size != b[1].size || made_up
    arity = a[1].size - 1
    # The signature has no entry for a block parameter, which facts lists last.
    names = [arity, arity + 1].include?(d[:params].size) ? d[:params] : []
    a[1].zip(b[1]).each_with_index.reject { |(x, y), _| x == y }.map do |(x, y), i|
      root = i == arity ? base : "#{base}(#{names[i] || "##{i + 1}"})"
      slot.([rbs_text(x)], [rbs_text(y)], root, i == arity ? copy : nil)
    end
  end

  def def_root(d) = "#{d[:owner].to_s.delete_suffix(".singleton")}#{d[:singleton] ? "." : "#"}#{d[:name]}"

  # The root a differing node of the main file belongs to. A parameter
  # record whose name the compiler changed is a block's, not the method's.
  def root_name(at, layout, renamed)
    d = layout.def_at[at[:line]]
    scope = layout.class_at[at[:line]]
    owner = (d ? d[:owner] : scope && scope[:name]).to_s.delete_suffix(".singleton")
    return "#{owner}#{at[:name]}" if at[:kind].start_with?("InstanceVariable")
    parameter = at[:kind].end_with?("ParameterNode")
    if d
      own_parameter = parameter && !renamed && d[:params].include?(at[:name])
      return own_parameter ? "#{def_root(d)}(#{at[:name]})" : def_root(d)
    end
    return "#{owner} (body)" if scope
    at[:name] && (parameter || at[:kind].start_with?("LocalVariable")) ? at[:name] : "(top level)"
  end
end
