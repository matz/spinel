# Literal probe: a program's string literals say what the text passes look for.
#
#   ruby tools/literal_probe.rb [--plant body|operand] [--needle own|searched]
#                               [--form string|symbol|interp].. [--bytes N]
#                               [--control] [--no-reduce] [--jobs J]
#                               [--out DIR] [--timeout SEC] [--keep] [FILE..]
#
# The text of a string literal is data. Spinel has passes that read text,
# though: the write barrier, the root elision and the root frame read the
# generated C, the call emitters search an arm's C for a name, and the
# parser reads the Ruby source for what a program mentions. A literal that
# spells what such a pass looks for is read as code. The corpus has almost
# no such literal, so no test sees it.
#
# The probe takes each FILE (default test/*.rb) and plants a string literal
# in void position at each site: with `--plant body` (the default) the head
# of every method, block and lambda body and of the top level, with `--plant
# operand` every receiver and argument that is a variable or a call, as
# `("..."; x)`. A first compile, with a marker in each literal, says which C
# function each lands in. Then each literal is given a needle. With `--needle
# own` (the default) it is that function's own lines: whatever a pass looks
# for in the C of this program, the program now also says as a string, in
# the function where it is looked for. With `--needle searched` it is every
# string spinel's sources search a text for, the literal arguments of the
# strstr, strncmp and memcmp calls under src/, read when the probe starts:
# what a function's C does not say, and a pass asks whether it says. Two
# compiles are compared: the needle, and a control of the same bytes with
# every character but blanks, quotes and backslashes turned to `x`. The two
# C files have to be equal outside the planted literals, and each planted
# literal has to read back, through C's escapes, as the text that was
# written. Every compile plants every site, so only text changes between
# two of them. Nothing is run and CRuby is not asked: the oracle is that
# data does not steer code.
#
# A form names the literal: `string` (the default) is "..." alone, `symbol`
# is :"...", which lands in the symbol table, and `interp` is "...#{nil}",
# whose text is copied by length. Several forms take the sites in turn.
# --control compiles the control twice, and has to find nothing but the
# class `control` below, which the control shows alone.
#
# A program that differs is reduced (delta debugging over the sites, then
# over the words and the characters of the needle) to the shortest literal
# that still makes a difference, its trigger: `->iv_leaf=`, `setjmp`, `#`.
# The places the needles spell it are then turned to control text and the
# program is asked again, so one program gives each of its triggers.
# Triggers alike but for a program's own names are one family. A family
# met in one program is asked about first in the next, by planting its
# trigger alone wherever the needles spell it, and only the smallest
# program of each family is reduced, built and run in both versions. A
# finding is classed:
#
#   literal  the planted literal reads back as other text: the program
#            holds a string it did not write
#   code     the C outside the literal changed: a barrier, a root, a branch
#            or a builtin was added or dropped
#   control  the control itself reads back as other text, or does not
#            compile: text that spells nothing was cut or rewritten
#   outcome  the needle is refused, or crashes the compiler, where the
#            control compiles
#
# Output, under DIR (default build/literal-probe): summary.txt and, for each
# family, findings/<n>-<class>/ with a.rb (the control), b.rb (the needle),
# note.txt (the trigger, the first difference, what the two builds did) and
# programs.txt (every program of the family, with what its needles spell).
#
# A probe to run by hand, like order_probe: not a gate, and not one of the
# tools make builds. It needs Prism, which Ruby 3.3 and later bundle.
#
# Exit status: 0 no finding, 1 at least one, 4 the tool's own error.

require "etc"
require "fileutils"
require "prism"
require "tmpdir"
require_relative "probe_common"

module LiteralProbe
  NAME = "literal_probe".freeze
  TAG = "lprobe_".freeze
  PLANTS = %w[body operand].freeze
  NEEDLES = %w[own searched].freeze
  FORMS = %w[string symbol interp].freeze
  CLASSES = %w[literal code control outcome].freeze
  # the prefixes spinel puts before a program's own names in the C
  NAMED = /\A(iv_|lv_|_cell_|c_|gv_|cst_)\w+\z/
  # how many triggers are looked for in one program
  TRIGGERS = 12
  # a known trigger that made a difference in fewer than one in RARE of the
  # programs spelling it is asked about together with the others like it
  RARE = 5
  # how many programs of a family are built and run in both versions
  RUNS = 20
  # how many pairs of compiles the reduction of one finding may take
  BUDGET = 400
  # the calls of spinel's sources that search a text for a string
  SEARCH = /\b(?:strstr|strncmp|memcmp)\s*\(/
  # What an operand site wraps: a value read or computed where it stands. A
  # literal is left alone, since spinel reads many at compile time (`require
  # "x"`, `attr_reader :x`, a format), and so are a splat, a block argument
  # and keywords, which parentheses do not hold.
  OPERANDS = [Prism::LocalVariableReadNode, Prism::InstanceVariableReadNode,
              Prism::GlobalVariableReadNode, Prism::CallNode].freeze

  # Prism reported errors for the source.
  class ParseError < StandardError; end

  # A place a literal is planted: the byte offset it goes in at. close is
  # the offset its closing parenthesis goes in at, for an operand and for
  # the body of a `def f = expr`. kind is "top", "def", "block" or
  # "operand".
  Site = Struct.new(:id, :offset, :close, :kind)

  # A trigger of a program. texts are the literals that make the difference,
  # by site id; whole says they are every place the needles spell a known
  # trigger, not yet reduced; key names the family; want and got are the two
  # sides of the first difference; run is what the two builds did.
  Finding = Struct.new(:path, :klass, :texts, :key, :want, :got, :whole, :run, :source, :sites)

  # A family met in an earlier program: its trigger as a pattern, its name,
  # how many programs spelling it were asked and in how many it made a
  # difference.
  Known = Struct.new(:pattern, :key, :tried, :shown)

  module_function

  # The sites of `source`, in source order.
  def sites(source, plant = "body")
    result = Prism.parse(source.b)
    raise ParseError, result.errors.first.message unless result.errors.empty?
    out = []
    visit = lambda do |node|
      case node
      when Prism::ProgramNode then body_site(out, node.statements, "top") if plant == "body"
      when Prism::DefNode then body_site(out, node.body, "def", !node.equal_loc.nil?) if plant == "body"
      when Prism::BlockNode, Prism::LambdaNode then body_site(out, node.body, "block") if plant == "body"
      when Prism::CallNode
        [node.receiver, *node.arguments&.arguments].each do |arg|
          next unless plant == "operand" && OPERANDS.include?(arg.class)
          out << Site.new(nil, arg.location.start_offset, arg.location.end_offset, "operand")
        end
      end
      node.compact_child_nodes.each(&visit)
    end
    visit.(result.value)
    out.sort_by! { |s| [s.offset, -s.close.to_i] }.each_with_index { |s, i| s.id = i }
  end

  def body_site(out, body, kind, endless = false)
    body = body.statements if body.is_a?(Prism::BeginNode)
    return unless body.is_a?(Prism::StatementsNode) && !body.body.empty?
    out << Site.new(nil, body.body.first.location.start_offset,
                    endless ? body.body.last.location.end_offset : nil, kind)
  end

  # `text` as a double-quoted Ruby literal.
  def quoted(text)
    "\"#{text.gsub(/[\\"#]/) { |ch| "\\#{ch}" }.gsub("\n", "\\n").gsub("\t", "\\t")}\""
  end

  # What a site's literal says: its tag, then the text.
  def said(id, text) = "#{TAG}#{id}_ #{text}".rstrip

  # The source with a literal planted at every site, in the form forms[id]:
  # the site's tag, and texts[id] where the site has one. Every compile of a
  # program plants every site, so that between two of them only text changes.
  def plant(source, sites, texts, forms)
    edits = []
    sites.each do |s|
      lit = quoted(said(s.id, texts[s.id].to_s))
      stmt = case forms[s.id]
             when "symbol" then ":#{lit}; "
             when "interp" then "#{lit.chop}\#{nil}\"; "
             else "#{lit}; "
             end
      edits << [s.offset, 1, s.id, s.close ? "(#{stmt}" : stmt]
      edits << [s.close, 0, -s.id, ")"] if s.close
    end
    # at one offset a close goes before an open, an outer open before an
    # inner one and an inner close before an outer one
    out = source.b
    edits.sort.reverse_each { |at, _, _, text| out.insert(at, text.b) }
    out
  end

  ESCAPES = { "n" => "\n", "t" => "\t", "r" => "\r", "a" => "\a", "b" => "\b",
              "f" => "\f", "v" => "\v", "e" => "\e" }.freeze

  # The C string literal whose text goes on at `pos`: the offset of its
  # closing quote and the bytes it stands for. Adjacent literals are one, as
  # they are to the C compiler.
  def c_literal(c, pos)
    out = +"".b
    i = pos
    loop do
      while i < c.bytesize && (ch = c.getbyte(i)) != 0x22
        if ch != 0x5c
          out << ch
          i += 1
        elsif (m = c.byteslice(i + 1, 3)[/\A[0-7]{1,3}/])
          out << (m.to_i(8) & 0xff)
          i += 1 + m.size
        elsif c.getbyte(i + 1) == 0x78
          m = c.byteslice(i + 2, 16)[/\A\h*/]
          out << (m.to_i(16) & 0xff)
          i += 2 + m.size
        else
          ch = c.byteslice(i + 1, 1).to_s
          out << (ESCAPES[ch] || ch)
          i += 2
        end
      end
      j = i + 1
      j += 1 while [0x20, 0x0a, 0x09].include?(c.getbyte(j))
      return [i, out] unless i < c.bytesize && c.getbyte(j) == 0x22
      i = j + 1
    end
  end

  STATEMENT = /\A(?:if|else|while|for|switch|do)\b/

  # The strings spinel's sources search a text for: each string literal
  # among the arguments of a strstr, strncmp or memcmp in a .c file of `dir`.
  def searched(dir)
    Dir.glob(File.join(dir, "*.c")).sort.flat_map do |file|
      text = File.binread(file)
      text.enum_for(:scan, SEARCH).flat_map do
        i = Regexp.last_match.end(0)
        depth = 1
        found = []
        while depth.positive? && i < text.bytesize
          case text[i]
          when "(" then depth += 1
          when ")" then depth -= 1
          when "'" then i += text[i + 1] == "\\" ? 3 : 2
          when "\""
            i, read = c_literal(text, i + 1)
            found << read
          end
          i += 1
        end
        found
      end
    end.uniq.grep(/\A[\x20-\x7e]+\z/).sort
  end

  # Each site's needle, for the sites whose marker reached the C: `words`
  # when given, else the lines of the C function the marker landed in, one
  # of each shape (a line with its numbers folded), `cap` bytes at most.
  def needles(c, sites, cap, words = nil)
    lines = c.lines
    starts = []
    lines.inject(0) { |off, l| starts << off; off + l.bytesize }
    sites.each_with_object({}) do |s, texts|
      pos = c.index("#{TAG}#{s.id}_") or next
      next texts[s.id] = words if words
      at = (starts.bsearch_index { |st| st > pos } || lines.size) - 1
      top = at
      top -= 1 while top > 0 && !(lines[top] =~ /\A[A-Za-z_].*\(.*\{\s*\z/ && lines[top] !~ STATEMENT)
      bottom = at
      bottom += 1 while bottom < lines.size - 1 && !lines[bottom].start_with?("}")
      seen = {}
      picked = []
      size = 0
      lines[(top + 1)...bottom].to_a.each do |l|
        l = l.strip.gsub(/[^\x20-\x7e]/, "?")
        next if l.empty? || l.include?(TAG) || seen[shape = l.gsub(/\d+/, "0")]
        break if size + l.bytesize + 1 > cap
        seen[shape] = true
        picked << l
        size += l.bytesize + 1
      end
      texts[s.id] = picked.join(" ") unless picked.empty?
    end
  end

  # The control of a needle: its bytes with nothing left to read as code.
  def control(text) = text.gsub(/[^\s"\\]/, "x")

  # Where two Strings first differ.
  def split(a, b)
    lo = 0
    hi = [a.bytesize, b.bytesize].min
    while lo < hi
      mid = (lo + hi) / 2
      a.byteslice(0, mid + 1) == b.byteslice(0, mid + 1) ? lo = mid + 1 : hi = mid
    end
    lo
  end

  # The two texts around the place they first differ.
  def sides(a, b, base_a = 0, base_b = 0, before: 50)
    k = split(a.byteslice(base_a..).to_s, b.byteslice(base_b..).to_s)
    [[a, base_a], [b, base_b]].map do |text, base|
      from = [base + k - before, 0].max
      text.byteslice(from, base + k - from + 110).to_s.gsub("\n", "\\n")
    end
  end

  # Where the control's C holds each planted literal, as [from, to, id], or
  # ["control", want, got] when one does not read back as it was written.
  def regions(ca, fillers)
    out = []
    fillers.each_key do |id|
      tag = "#{TAG}#{id}_"
      pos = -1
      while (pos = ca.index(tag, pos + 1))
        fin, read = c_literal(ca, pos)
        return ["control", *sides(said(id, fillers[id]), read, before: 30)] unless read == said(id, fillers[id])
        out << [pos, fin, id]
      end
    end
    out.sort
  end

  # The needle's C against the control's: nil when it is the control's with
  # each planted literal saying its needle, else [class, want, got].
  def compare(ca, cb, regions, texts)
    pa = pb = 0
    regions.each do |from, to, id|
      piece = ca.byteslice(pa, from - pa)
      return ["code", *sides(ca, cb, pa, pb)] unless cb.byteslice(pb, piece.bytesize) == piece
      fin, read = c_literal(cb, pb + piece.bytesize)
      # another literal in its place (a symbol table in another order) is code
      return ["code", *sides(ca, cb, pa, pb)] unless read.include?("#{TAG}#{id}_")
      return ["literal", *sides(said(id, texts[id]), read, before: 30)] unless read == said(id, texts[id])
      pa = to
      pb = fin
    end
    ca.byteslice(pa..) == cb.byteslice(pb..) ? nil : ["code", *sides(ca, cb, pa, pb)]
  end

  # A smallest sublist of `items` the block still answers true for (ddmin).
  def ddmin(items)
    n = 2
    while items.size >= 2
      chunks = items.each_slice((items.size / n.to_f).ceil).to_a
      keep = chunks.find { |ch| yield ch }
      if keep
        items = keep
        n = 2
        next
      end
      keep = chunks.size > 2 && chunks.each_index.lazy.map { |i| (chunks[0...i] + chunks[(i + 1)..]).flatten(1) }
                                     .find { |rest| yield rest }
      if keep
        items = keep
        n = [n - 1, 2].max
      elsif n >= items.size then break
      else n = [n * 2, items.size].min
      end
    end
    items
  end

  # A trigger as a pattern: a program's own name behind one of spinel's
  # prefixes is any name, a one-letter word any letter (the reduction keeps
  # one letter where any name would do), a number any number, blanks optional.
  def pattern(trigger)
    Regexp.new(trigger.scan(/\w+|[^\w\s]/).map do |tok|
      if (m = NAMED.match(tok)) then "#{m[1]}\\w+"
      elsif tok =~ /\A[A-Za-z]\z/ then "\\w"
      else Regexp.escape(tok).gsub(/\d+/, "\\d+")
      end
    end.join("\\s*"))
  end

  # The name of a trigger's family, by the same rules.
  def family(trigger)
    trigger.scan(/\w+|[^\w\s]/).map do |tok|
      if (m = NAMED.match(tok)) then "#{m[1]}NAME"
      elsif tok =~ /\A[A-Za-z]\z/ then "x"
      else tok.gsub(/\d+/, "N")
      end
    end.join(" ").gsub(/ ?([^\w\s]) ?/, "\\1")
  end

  class Probe
    attr_reader :findings

    def initialize(spinel, root, work, timeout:, plant:, forms:, cap:, words:, control:, reduce:)
      @spinel = spinel
      @root = root
      @work = work
      @timeout = timeout
      @plant = plant
      @forms = forms
      @cap = cap
      @words = words
      @control = control
      @reduce = reduce
      @findings = []
      @skips = Hash.new { |h, k| h[k] = [] }
      @known = {} # the families met so far, by pattern
      @planted = @bytes = @compiles = 0
      @lock = Mutex.new
      @stopped = false
      @halt = -> { @stopped }
    end

    # Stops the probe: every compile in flight is killed, and it and every
    # later one raise ProbeCommon::Stopped.
    def stop
      @stopped = true
    end

    # Waits for `threads`; an interrupt stops them first (see order_probe).
    def finish(threads)
      threads.each(&:join)
    ensure
      if threads.any?(&:alive?)
        stop
        threads.each(&:join)
      end
    end

    def relative(path) = path.delete_prefix("#{@root}/")

    def skip(path, why)
      @lock.synchronize { @skips[why] << path }
    end

    def forms(sites) = sites.to_h { |s| [s.id, @forms[s.id % @forms.size]] }

    # The C of `src`, or nil and what became of the compile.
    def compile(src)
      out = "#{src}.c"
      FileUtils.rm_f(out)
      log = "#{src}.log"
      status, timed_out = ProbeCommon.run_timed([@spinel, "-c", "--no-line-map", src, "-o", out],
                                                @timeout, log, log, @halt)
      @lock.synchronize { @compiles += 1 }
      return [nil, "does not compile in time"] if timed_out
      return [nil, "crashes the compiler"] if status.signaled?
      return [nil, "is refused"] unless status.success? && File.exist?(out)
      [File.binread(out), nil]
    end

    # One program: the marker, the control and the needle compiled, the last
    # two compared, and each trigger of a difference found. `id` names its
    # scratch directory; the three are compiled from one path there, since
    # the C holds a program's path.
    def check(path, id)
      source = File.binread(path)
      begin
        sites = LiteralProbe.sites(source, @plant)
      rescue ParseError
        return skip(path, "ruby #{RUBY_VERSION} does not parse it")
      end
      return skip(path, "nowhere to plant") if sites.empty?
      dir = File.join(@work, id.to_s)
      FileUtils.mkdir_p(dir)
      src = File.join(dir, File.basename(path))
      pair = pairer(source, sites, src)
      File.binwrite(src, LiteralProbe.plant(source, sites, {}, forms(sites)))
      marked, why = compile(src)
      # compiled from the scratch directory it cannot read a file beside
      # itself; a planted body is also no longer the one-line reader spinel
      # knows by its shape
      return skip(path, "the planted program #{why}") unless marked
      texts = LiteralProbe.needles(marked, sites, @cap, @words)
      return skip(path, "no planted literal reaches the C") if texts.empty?
      @lock.synchronize do
        @planted += texts.size
        @bytes += texts.each_value.sum(&:bytesize)
      end
      # each trigger in turn: one is found, every place the needles spell it
      # is turned to control text, and what is left is compared again
      seen = []
      dead = [] # the known families that make no difference in this program
      while !texts.empty? && (diff = pair.(texts))
        # a control that is not read back is reduced as its own needle
        mine = diff[0] == "control" ? texts.transform_values { |t| LiteralProbe.control(t) } : texts
        f = Finding.new(path, diff[0], mine, "(not reduced)", diff[1], diff[2], false, nil, source, sites)
        pat = reduce(f, pair, dead) if @reduce
        break if pat && seen.include?(pat) # the pattern did not cover every spelling of its trigger
        @lock.synchronize { @findings << f }
        seen << pat
        break if seen.size == TRIGGERS
        if pat then texts = texts.transform_values { |t| t.gsub(pat) { |hit| LiteralProbe.control(hit) } }
        elsif f.klass == "control" then texts = texts.except(*f.texts.keys)
        else break
        end
      end
    ensure
      FileUtils.rm_rf(dir) if dir
    end

    # A pair of compiles of one program, as a lambda from the texts to the
    # difference they make: [class, want, got] (want is nil where there is no
    # text to show) or nil. The control compiled last is kept, since a needle
    # with places turned to control text shares it.
    def pairer(source, sites, src)
      forms = forms(sites)
      was = ca = at = nil
      lambda do |texts|
        fillers = texts.transform_values { |t| LiteralProbe.control(t) }
        unless fillers == was
          File.binwrite(src, LiteralProbe.plant(source, sites, fillers, forms))
          ca, why = compile(src)
          # set either way: the regions of an earlier control are not this one's
          at = ["control", nil, "the control #{why}, the marker alone compiles"]
          at = LiteralProbe.regions(ca, fillers) if ca
          was = fillers
        end
        next at if at.first == "control"
        File.binwrite(src, LiteralProbe.plant(source, sites, @control ? fillers : texts, forms))
        cb, why = compile(src)
        next ["outcome", nil, "the needle #{why}, the control compiles"] unless cb
        LiteralProbe.compare(ca, cb, at, @control ? fillers : texts)
      end
    end

    # Finds the trigger of f, a difference `pair` shows, and answers its
    # pattern (nil when it has none: several literals are needed, or any
    # text of that length does it). The families met so far are asked
    # first, and one of them is taken without reducing: the smallest program
    # of each family is reduced at the end.
    def reduce(f, pair, dead)
      own = f.klass == "control"
      shows = lambda do |texts|
        diff = pair.(texts) or next false
        # a control that fails for a part of the texts says nothing of them
        next false unless own == (diff[0] == "control")
        f.klass, f.want, f.got = diff
        true
      end
      known = recognize(f, shows, dead) unless own
      return known.pattern if known
      minimize(f, shows)
      f.key = "(text that spells nothing)"
      return nil if f.klass == "control"
      f.key = "(several literals together)"
      return nil unless f.texts.size == 1
      f.key = LiteralProbe.family(f.texts.values.first)
      pat = LiteralProbe.pattern(f.texts.values.first)
      @lock.synchronize do
        known = (@known[pat] ||= Known.new(pat, f.key, 0, 0))
        known.tried += 1
        known.shown += 1
      end
      pat
    end

    # The family met before whose trigger, planted alone wherever f's texts
    # spell it, makes a difference here; f then holds those texts. The
    # families that mostly do are asked one by one, the most frequent first;
    # the ones that seldom do are first asked all together, and one by one
    # only where that makes a difference. One that makes none is put on
    # `dead` and not asked of this program again.
    def recognize(f, shows, dead)
      spelled = lambda do |families|
        f.texts.transform_values { |t| families.flat_map { |k| t.scan(k.pattern) }.uniq.join(" ") }
               .reject { |_, t| t.empty? }
      end
      asks = lambda do |known|
        hit = shows.(texts = spelled.([known]))
        @lock.synchronize do
          known.tried += 1
          known.shown += 1 if hit
        end
        dead << known unless hit
        f.texts, f.key, f.whole = texts, known.key, true if hit
        hit
      end
      often, seldom = @lock.synchronize do
        @known.values.sort_by { |k| -k.shown }.partition { |k| k.shown * RARE >= k.tried }
      end.map { |part| part.reject { |k| dead.include?(k) || spelled.([k]).empty? } }
      found = often.find(&asks)
      return found if found
      if seldom.size > 1 && !shows.(spelled.(seldom))
        dead.concat(seldom)
        return nil
      end
      seldom.find(&asks)
    end

    # Shrinks f.texts to the fewest sites and, where that is one site, to the
    # shortest text that still makes a difference (delta debugging over the
    # sites, then over the words, the tokens and the characters of the
    # text). It stops where it is after BUDGET pairs of compiles.
    def minimize(f, asks)
      left = BUDGET
      shows = ->(texts) { (left -= 1) >= 0 && asks.(texts) }
      ids = LiteralProbe.ddmin(f.texts.keys) { |keep| shows.(f.texts.slice(*keep)) }
      texts = f.texts.slice(*ids)
      texts.each_key do |id|
        break unless texts.size == 1
        if f.klass == "control" # the shortest text that is still cut
          size = (1..texts[id].size).bsearch { |n| shows.(texts.merge(id => texts[id][0, n])) }
          texts = texts.merge(id => texts[id][0, size]) if size
          next
        end
        [/\S+\s*/, /\w+|\s+|[^\w\s]/, /./m].each do |unit|
          parts = LiteralProbe.ddmin(texts[id].scan(unit)) { |keep| shows.(texts.merge(id => keep.join)) }
          texts = texts.merge(id => parts.join)
        end
      end
      asks.(texts) # the last compare made may be of a text that was put back
      f.texts = texts
      f.whole = false
    end

    # Reduces a finding that was taken whole, keeping its class and family.
    def settle(f, dir)
      return unless f.whole
      pair = pairer(f.source, f.sites, File.join(dir, File.basename(f.path)))
      minimize(f, lambda do |texts|
        diff = pair.(texts)
        next false unless diff && diff[0] == f.klass
        f.want, f.got = diff[1], diff[2]
        true
      end)
    end

    # Builds and runs both versions of f's program, written under `dir`:
    # a.rb, the control (for a control that is itself the finding, the marker
    # alone), and b.rb. f.run is what they did, "alike" where they ended
    # the same way having printed the same.
    def run(f, dir)
      controls = f.texts.transform_values { |t| f.klass == "control" ? "" : LiteralProbe.control(t) }
      a, b = [["a", controls], ["b", f.texts]].map do |tag, texts|
        src = File.join(dir, "#{tag}.rb")
        File.binwrite(src, LiteralProbe.plant(f.source, f.sites, texts, forms(f.sites)))
        bin = "#{src}.bin"
        log = "#{src}.log"
        status, timed_out = ProbeCommon.run_timed([@spinel, src, "-o", bin], @timeout * 4, log, log, @halt)
        next "does not build" if timed_out || !status.success?
        status, timed_out = ProbeCommon.run_timed([bin], @timeout, "#{src}.out", log, @halt)
        next "does not end" if timed_out
        [status.signaled? ? "signal #{status.termsig}" : "exit #{status.exitstatus}", File.binread("#{src}.out")]
      end
      # a program that prints its own source or name prints the planted text
      own = [a, b].any? { |did| did.is_a?(Array) && (did[1].include?(TAG) || did[1] =~ /\b[ab]\.rb\b/) }
      f.run = if a == b then "alike (#{a.is_a?(Array) ? a[0] : a})"
              elsif a.is_a?(String) then "a.rb #{a}"
              elsif b.is_a?(String) then "b.rb #{b}, a.rb runs (#{a[0]})"
              elsif a[0] != b[0] then "a.rb ends with #{a[0]}, b.rb with #{b[0]}"
              elsif own then "alike (#{a[0]}) but for the program's own text, which it prints"
              else "the two print different output"
              end
    ensure
      FileUtils.rm_f(Dir.glob(File.join(dir, "?.rb.*")))
    end

    # The findings by family, the largest first within a class, the smallest
    # program first within a family.
    def families
      @families ||= @findings.group_by { |f| [f.klass, f.key] }
                             .sort_by { |(klass, key), fs| [CLASSES.index(klass), -programs(fs), key] }
                             .each { |_, fs| fs.sort_by! { |f| [f.source.bytesize, f.path] } }
    end

    def programs(fs) = fs.map(&:path).uniq.size

    # The programs of a family that are run: all of them up to RUNS, else
    # RUNS of them spread evenly by size, the smallest first.
    def sample(fs)
      one = fs.uniq(&:path)
      return one if one.size <= RUNS
      Array.new(RUNS) { |i| one[i * (one.size - 1) / (RUNS - 1)] }
    end

    # What the runs of a family did.
    def ran(fs)
      did = fs.select(&:run)
      odd = did.reject { |f| f.run.start_with?("alike") }
      "#{did.size} run, #{odd.empty? ? "all alike" : "#{odd.size} not alike: #{odd.first.run}"}"
    end

    def trigger(f)
      if f.whole
        said = f.texts.values.join(" ")
        said = "#{said[0, 80]}.." if said.bytesize > 80
        return "#{said.inspect} in #{f.texts.size} literal#{"s" unless f.texts.size == 1}, not reduced"
      end
      f.texts.first(3).map do |id, t|
        where = f.sites[id].kind == "operand" ? "an operand" : "a #{f.sites[id].kind} body"
        text = t.bytesize > 80 ? "#{t[0, 80]}.." : t
        "#{f.klass == "control" ? "#{t.bytesize} bytes, " : ""}#{text.inspect} in #{where}"
      end.join(", ") + (f.texts.size > 3 ? " and #{f.texts.size - 3} more" : "")
    end

    # Writes each family's directory. The smallest program of a family is
    # reduced and kept as a.rb and b.rb; it and the rest of the family's
    # sample are built and run in both versions, `jobs` at a time.
    def write_findings(out, jobs)
      dirs = families.each_index.map { |n| File.join(out, "findings", format("%02d-%s", n + 1, families[n][0][0])) }
      queue = Queue.new
      families.each_with_index do |(_, fs), n|
        FileUtils.mkdir_p(dirs[n])
        sample(fs).each_with_index { |f, i| queue << [f, (dirs[n] if i.zero?)] }
      end
      finish(Array.new(jobs) do
        Thread.new do
          while (f, dir = (queue.pop(true) rescue nil))
            scratch = Dir.mktmpdir("run", @work)
            begin
              settle(f, scratch) if dir
              run(f, dir || scratch)
            rescue ProbeCommon::Stopped
              break
            rescue StandardError => e
              # one program the tool cannot run is one run that is not alike
              f.run = "tool error: #{e.class}: #{e.message.lines.first.to_s.strip}"
            ensure
              FileUtils.rm_rf(scratch)
            end
          end
        end
      end)
      families.each_with_index do |((klass, key), fs), n|
        f = fs.first
        File.write(File.join(dirs[n], "note.txt"), <<~NOTE)
          family: #{key} (#{klass}, #{programs(fs)} programs)
          program: #{relative(f.path)}
          trigger (b.rb): #{trigger(f)}
          difference, a.rb's side and then b.rb's:
            #{[f.want, f.got].compact.join("\n  ")}
          run: #{f.run}
          of the family: #{ran(fs)}
        NOTE
        File.write(File.join(dirs[n], "programs.txt"),
                   fs.map { |g| "#{relative(g.path)}\t#{trigger(g)}\t#{g.run}\n" }.join)
      end
    end

    def summary(total)
      s = +"programs: #{total}, #{total - @skips.each_value.sum(&:size)} compared " \
           "(#{@planted} literals holding #{@bytes} bytes of needle, #{@compiles} compiles)\n"
      @skips.sort_by { |why, ps| [-ps.size, why] }.each do |why, ps|
        names = ps.sort.first(3).map { |p| File.basename(p) }.join(", ")
        s << "left out #{ps.size}: #{why} (#{names}#{ps.size > 3 ? ", .." : ""})\n"
      end
      s << "findings: #{@findings.map(&:path).uniq.size} programs, #{families.size} families\n"
      families.each_with_index do |((klass, key), fs), n|
        f = fs.first
        s << format("  %02d %-8s %5d  %-22s %s\n", n + 1, klass, programs(fs), key, ran(fs))
        next if klass == "code"
        s << format("%44s%s\n", "", f.want ? "#{f.want.inspect} is read as #{f.got.inspect}" : f.got)
      end
      s
    end
  end

  def main(argv)
    root = File.expand_path("..", __dir__)
    out = File.join(root, "build", "literal-probe")
    plant = "body"
    needle = "own"
    forms = []
    cap = 4000
    jobs = Etc.nprocessors
    timeout = 60
    control = keep = false
    reduce = true
    files = []
    begin
      until argv.empty?
        case (a = argv.shift)
        when "--plant" then plant = argv.shift
        when "--needle" then needle = argv.shift
        when "--form" then forms << argv.shift
        when "--bytes" then cap = Integer(argv.shift)
        when "--control" then control = true
        when "--no-reduce" then reduce = false
        when "--jobs" then jobs = Integer(argv.shift)
        when "--out" then out = File.expand_path(argv.shift)
        when "--timeout" then timeout = Integer(argv.shift)
        when "--keep" then keep = true
        when /\A--/ then raise ArgumentError, "unknown option #{a}"
        else files << File.expand_path(a)
        end
      end
      bad = ([plant] - PLANTS) + ([needle] - NEEDLES) + (forms - FORMS)
      raise ArgumentError, "unknown --plant, --needle or --form #{bad.first.inspect}" unless bad.empty?
      positive = [cap, jobs, timeout].all?(&:positive?)
      raise ArgumentError, "--bytes, --jobs and --timeout take a positive number" unless positive
    rescue ArgumentError, TypeError => e
      warn "#{NAME}: #{e.message}"
      return 4
    end
    forms = ["string"] if forms.empty?
    spinel = File.join(root, "bin", "spinel")
    unless File.executable?(spinel)
      warn "#{NAME}: #{spinel} is not built; run make"
      return 4
    end
    files = Dir.glob(File.join(root, "test", "*.rb")).sort if files.empty?
    missing = files.reject { |f| File.file?(f) }
    unless missing.empty?
      warn "#{NAME}: no such file: #{missing.first}"
      return 4
    end
    # the promote tests are another language level, compiled under a flag
    files = files.reject { |f| File.basename(f).start_with?("promote_") } unless files.size == 1
    if File.directory?(out) && !Dir.empty?(out) && %w[summary.txt .lock].none? { |f| File.exist?(File.join(out, f)) }
      warn "#{NAME}: #{out} holds files the tool did not write; give --out an empty or new directory"
      return 4
    end
    FileUtils.mkdir_p(out)
    dir_lock = File.open(File.join(out, ".lock"), File::RDWR | File::CREAT)
    unless dir_lock.flock(File::LOCK_EX | File::LOCK_NB)
      warn "#{NAME}: another run is using #{out}; give --out another directory"
      return 4
    end

    work = nil
    Thread.report_on_exception = false
    begin
      %w[findings work summary.txt].each { |p| FileUtils.rm_rf(File.join(out, p)) }
      File.write(File.join(out, "summary.txt"), "run in progress\n")
      work = keep ? File.join(out, "work") : Dir.mktmpdir("literal-probe")
      FileUtils.mkdir_p(work)
      words = searched(File.join(root, "src")) if needle == "searched"
      probe = Probe.new(spinel, root, work, timeout: timeout, plant: plant, forms: forms.uniq, cap: cap,
                                            words: words&.join(" "), control: control, reduce: reduce)
      queue = Queue.new
      files.each_with_index { |f, i| queue << [f, i] }
      done = 0
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      progress = Mutex.new
      workers = Array.new(jobs) do
        Thread.new do
          while (f, i = (queue.pop(true) rescue nil))
            begin
              probe.check(f, i)
            rescue ProbeCommon::Stopped
              break
            rescue StandardError => e
              # one program the tool cannot read is one program left out
              probe.skip(f, "tool error: #{e.class}: #{e.message.lines.first.to_s.strip}")
            end
            progress.synchronize do
              done += 1
              $stderr.print "\r#{done}/#{files.size} programs, #{probe.findings.size} findings"
            end
          end
        end
      end
      probe.finish(workers)
      $stderr.puts
      probe.write_findings(out, jobs)
      took = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
      version = IO.popen([spinel, "--version"], err: File::NULL, &:read).strip
      needle = words ? "the #{words.size} strings src/*.c searches for" : "own C, #{cap} bytes"
      what = "plant: #{plant}, needle: #{needle}, forms: #{forms.uniq.join(" ")}" \
             "#{control ? ", control: the control compiled twice" : ""}"
      report = "spinel: #{version}\nruby: #{RUBY_DESCRIPTION}\n#{what}\n" +
               probe.summary(files.size) + format("wall time: %ds (jobs %d)\n", took, jobs)
      File.write(File.join(out, "summary.txt"), report)
      puts report
      puts "findings under #{out}" unless probe.findings.empty?
      probe.findings.empty? ? 0 : 1
    rescue ProbeCommon::Stopped, Interrupt
      warn "#{NAME}: interrupted"
      4
    rescue StandardError => e
      warn "#{NAME}: #{e.message}"
      4
    ensure
      FileUtils.rm_rf(work) if work && !keep
      dir_lock.close
    end
  end
end

exit LiteralProbe.main(ARGV) if $PROGRAM_NAME == __FILE__
