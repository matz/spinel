#!/usr/bin/env ruby
# frozen_string_literal: true

# Which emitters produced a call's C, and which arm of the call chain took it;
# with --analysis, which pass and which rule gave a node its facts.
#
#   make build/spinel-trace
#   ruby tools/emit_trace.rb PROG.rb [LINE]
#   ruby tools/emit_trace.rb --analysis PROG.rb LINE
#
# Runs the trace build of the compiler on PROG.rb (src/emit_trace.c has the
# record format) and prints, for each call and statement the code generator
# started on LINE (every line without it), the tree of functions that emitted
# it. Offsets are named with the binary's own symbol table (`nm -n`), which
# lists static functions too. With LINE the build records that line alone.
#
# - Only functions with one of the prefixes below are shown; --prefix adds
#   one. The callees of a function that is not an emitter (emit_*) are shown
#   only where they reach one, so a plan or layout question is one line.
#   --all shows every function of src/, at every depth.
# - A function called several times in a row with the same callees prints
#   once, with (xN).
# - A call or statement emitted inside another has a tree of its own; the
#   outer tree shows it as one line, with its position in brackets, and its
#   own head names the arm of the enclosing call it was emitted in. The
#   compiler emits some nodes more than once (an arm's trial, whose text it
#   drops when it declines; a method spliced in at several calls): each
#   emission has a tree.
# - "took the call" marks the last emitter emit_call_body_ordinary (or
#   emit_call_body) called: the chain is `if (arm) return;`, so that is the
#   arm that answered. An arm the chain calls through emit_or_take_back is
#   marked itself. When the chain's own code wrote the call, the mark is on
#   the last emitter that code used.
#
# --analysis prints, for each node of LINE, what the analysis decided about it
# in the order it did: its class set (the rules cf_node ran for it in each
# walk, and the set each walk and their meet left), its type (each change,
# with the pass, the fixpoint round, the callers and the tree of inference
# functions under infer_type) and its nil fact.
#
# - An inference that changed nothing is not in the trace. One that repeats
#   the one before it prints once, with the rounds it recurred in; a tree
#   printed earlier is referred to, not printed again.
# - "last rule called" marks the last infer_ (or nf_) function the inference
#   called before it returned. infer_uncached, infer_call_inner and
#   nf_expr_uncached are mostly chains of `if (rule) return`, so it is usually
#   the rule that answered; what the tree shows after the mark is the chain's
#   own code, which may have answered instead. Where that code went on to
#   another node's inference, the mark is on the chain itself.
# - An infer_type or nf_expr with a node in brackets computed that node's
#   fact inside this one: a node of LINE has its own tree, another line's is
#   that line's to ask about. Without a node it answered from its memo.

require "optparse"
require "tmpdir"

PREFIXES = %w[emit_ cplan_ repr_ bop_ call_plan call_target].freeze
EMITTER = "emit_"
CHAIN = %w[emit_call_body_ordinary emit_or_take_back].freeze # the mark goes to what these called
SITES = { "c".ord => ["call", "emit_call_body"], "s".ord => ["stmt", "emit_stmt"],
          "l".ord => ["tail", "emit_stmt_tail"] }.freeze # tail: the last statement of a method that answers it
# --analysis: a fact's name, the prefix of its rules, and the chain the mark goes down
FACTS = { "t".ord => ["type", "infer_", %w[infer_uncached infer_call infer_call_inner]],
          "n".ord => ["nil fact", "nf_", %w[nf_expr_uncached]], "k".ord => ["class set", "cf_", []] }.freeze
ANALYSIS_PREFIXES = %w[infer_ nf_ cf_ an_].freeze
ASKERS = %w[infer_type nf_expr cf_node].freeze # where a fact is asked: a leaf unless it carries a node
QUIET = /\Ainfer_memo_/ # the memo's bookkeeping under every infer_type
HEAD, NODE, ENTER, EXIT, TEXT, STACK, PASS, FACT = 1, 2, 3, 4, 5, 6, 7, 8
Tree = Struct.new(:name, :kids, :note) do
  def emits? = name.start_with?(EMITTER) || !note.nil? || kids.any?(&:emits?)
end
# tree: where this frame's callees attach, nil outside a traced node; ev (--analysis): the ask it computes
Frame = Struct.new(:off, :tree, :ev)
# --analysis: one computation of the fact of a node of LINE (with its tree), or one fact a pass wrote itself
Event = Struct.new(:callers, :pass, :round, :kind, :node, :tree, :facts)

prefixes = PREFIXES.dup
all = analysis = false
spinel = File.expand_path("../build/spinel-trace", __dir__)
opts = OptionParser.new("usage: emit_trace.rb [options] PROG.rb [LINE]") do |o|
  o.on("--analysis", "the passes and rules that gave LINE's nodes their facts") { analysis = true }
  o.on("--prefix P", "also show functions whose names start with P") { |p| prefixes << p }
  o.on("--all", "show every function, at every depth") { all = true }
end
opts.parse!
prog, line = ARGV
abort opts.to_s unless prog && ARGV.size <= 2 && (line || !analysis)
abort "emit_trace: #{spinel} is not built (make build/spinel-trace)" unless File.executable?(spinel)
line &&= Integer(line)
abort "emit_trace: lines count from 1" if line && line < 1
prefixes = ANALYSIS_PREFIXES + prefixes if analysis

# Function start address -> name. Mach-O puts an underscore before every C
# name; a compiler-made part of a function (.cold, .part) keeps its name.
addr = {}
by_name = {}
syms = IO.popen(["nm", "-n", spinel], &:readlines).filter_map do |l|
  a, type, name = l.split
  [a.to_i(16), name] if name && "tTwW".include?(type)
end
macho = syms.any? { |_, n| n == "__mh_execute_header" }
syms.each do |a, n|
  n = n.delete_prefix("_") if macho
  by_name[n] ||= a
  addr[a] ||= n.sub(/\..*/, "")
end

source = File.readlines(prog, chomp: true, encoding: "UTF-8")
text_at = lambda do |b, c| # (a column counts bytes)
  text = (b > 0 && source[b - 1] || "").byteslice(c..).to_s.scrub.strip
  text.size > 60 ? "#{text[0, 57]}..." : text
end
sections = [] # [position, label, source text, tree, emitted in], in the order the nodes were emitted
calls = {}.compare_by_identity # a call's tree => its position and label
events = [] # --analysis: in the order the analysis made them
nodes = {} # --analysis: id => [line, column, label, on LINE of PROG's own file]
ok = false
Dir.mktmpdir("emit_trace") do |dir|
  trace = File.join(dir, "trace")
  env = { "SPINEL_EMIT_TRACE" => trace, "SPINEL_EMIT_TRACE_LINE" => line&.to_s, # nil unsets it
          "SPINEL_EMIT_TRACE_ANALYSIS" => (analysis ? "1" : nil) }
  ok = system(env, spinel, prog, "-S", out: File::NULL)
  abort "emit_trace: no trace written" unless File.size?(trace)
  warn "emit_trace: the compile failed; the trace stops where it did" unless ok

  slide = nil # symbol table address minus trace offset
  label = nil # the last record's text, which its TEXT records complete
  stack = []
  ev = nil # --analysis: where the recording being read was made (its pass and round)
  # --analysis: the open functions, an asking one with its node
  callers = lambda do |frames|
    frames.map { |fr| "#{addr[fr.off + slide]}#{" [#{nodes[fr.ev.node][2]} #{nodes[fr.ev.node][0]}:#{nodes[fr.ev.node][1] + 1}]" if fr.ev}" }
  end
  File.open(trace, "rb") do |f|
    while (chunk = f.read(16 << 16))
      chunk.unpack("L*").each_slice(4) do |tag, a, b, c|
        off = b << 32 | a
        case tag & 0xff
        when HEAD
          slide = by_name.fetch("emit_trace_node") - off
        when ENTER
          at = stack.last&.tree
          name = addr[off + slide] || format("0x%x", off)
          at.kids << (at = Tree.new(name, [])) if at && (all || (name.start_with?(*prefixes) && name !~ QUIET))
          stack << Frame.new(off, at)
        when EXIT
          i = stack.rindex { |fr| fr.off == off } # a longjmp leaves frames without their exit
          stack.slice!(i..) if i
        when TEXT
          label << [a, b, c].pack("L3").delete("\0").force_encoding(label.encoding)
        when STACK
          stack.clear
          ev = Event.new(nil, nil, nil, nil, nil, nil, [])
        when PASS
          ev.round = a
          ev.pass = label = +""
        when FACT
          nodes[a] ||= [b, c, nil, b == line && tag >> 16 == 0]
          label = +"= "
          kind = tag >> 8 & 0xff
          if (at = stack.reverse_each.find(&:ev)) # inside a computation: a line of its tree, another node's with its place
            at.ev.facts << [a, label, kind] if nodes[a][3]
            stack.last.tree&.kids&.push(Tree.new(label, [], a == at.ev.node ? nil : ["#{b}:#{c + 1}"]))
          else # a pass's own write
            events << Event.new(callers[stack], ev.pass, ev.round, kind, a, nil, [[a, label, kind]])
          end
        when NODE
          if analysis
            label = +""
            mine = b == line && tag >> 16 == 0
            nodes[a] = [b, c, label, mine] unless nodes[a]&.[](2)
            frame = stack.last
            # a node computed inside another's computation is one line of that tree: a node of
            # LINE has a tree of its own, and another line's is that line's to ask about
            frame.tree&.kids&.clear
            frame.tree&.note = [label, "#{b}:#{c + 1}"]
            next frame.tree = nil unless mine
            from = callers[stack[0..-2]]
            frame.tree = Tree.new(addr[frame.off + slide], [], [])
            events << (frame.ev = Event.new(from, ev.pass, ev.round, tag >> 8 & 0xff, a, frame.tree, []))
            next
          end
          kind, fn = SITES.fetch(tag >> 8 & 0xff)
          # recording starts at a marker, after its function was entered
          stack << Frame.new(by_name.fetch(fn) - slide, nil) unless stack.last && addr[stack.last.off + slide] == fn
          frame = stack.last
          label = +"#{kind} "
          where = "#{b}:#{c + 1}"
          if (outer = stack[-2]&.tree)
            stub = frame.tree.equal?(outer) ? Tree.new(fn, []).tap { |t| outer.kids << t } : frame.tree
            stub.kids.clear
            stub.note = [label, where]
          end
          frame.tree = nil
          next if tag >> 16 != 0 || (line && b != line) # a required file's lines are not PROG's
          # the arm of the enclosing call this one is emitted in: the first function below its
          # chain (a call of this line, so that a line's output is the same with and without LINE)
          path = stack[0..-2].filter_map(&:tree).chunk_while { |x, y| x.equal?(y) }.map(&:first)
          i = path.rindex { |t| calls.key?(t) }
          arm = i && calls[path[i]][2] == b && path[(i + 1)..].find { |t| !CHAIN.include?(t.name) }
          frame.tree = Tree.new(fn, [])
          calls[frame.tree] = [where, label, b] if kind == "call" # (the label's TEXT records are still to come)
          sections << [where, label, text_at[b, c], frame.tree, arm && [arm.name, "of", *calls[path[i]][0, 2]]]
        end
      end
    end
  end
end
abort "emit_trace: nothing was #{analysis ? 'decided' : 'emitted'} for #{prog}#{":#{line}" if line}" if sections.empty? && events.empty?

# what a function called, as shown: nothing below a helper that reaches no emitter
shown = lambda do |tree|
  next ASKERS.include?(tree.name) && !tree.note ? [] : tree.kids if analysis
  all || tree.name.start_with?(EMITTER) || tree.kids.any?(&:emits?) ? tree.kids : []
end
# A function and what it called; one that repeats the one before it, callees and all, counts on its line
render = lambda do |tree, depth, took, mark, out|
  last = at = n = nil
  shown[tree].each do |k|
    render.call(k, depth + 1, took, mark, below = [])
    this = ["#{'  ' * depth}#{k.name}", k.note ? "  [#{k.note.join(' ')}]" : "", below]
    if this == last && !k.equal?(took)
      out[at] = "#{this[0]} (x#{n += 1})#{this[1]}"
      next
    end
    last, at, n = this, out.size, 1
    out << (k.equal?(took) ? "#{(this[0] + this[1]).ljust(47)} <- #{mark}" : this[0] + this[1])
    out.concat(below)
  end
end

last_emitter = ->(tree) { tree.kids.reverse.find { |k| k.name.start_with?(EMITTER) } }
sections.each_with_index do |(where, label, text, tree, arm), i|
  took = nil
  if tree.name == "emit_call_body"
    took = last_emitter[tree]
    took = last_emitter[took] || took while CHAIN.include?(took&.name) && last_emitter[took]
  end
  out = ["#{prog}:#{where}  #{label}  `#{text}`#{"  (in #{arm.join(' ')})" if arm}", "  #{tree.name}"]
  render.call(tree, 2, took, "took the call", out)
  puts if i > 0
  puts out
end

# --analysis: the last rule the inference called, down its chain of `if (rule) return`;
# the chain itself where its own code then asked for another node
last_rule = lambda do |ev|
  _, rule, chain = FACTS.fetch(ev.kind)
  t = ev.tree.kids.find { |x| x.name == chain[0] }
  while t && chain.include?(t.name)
    i = t.kids.rindex { |x| x.name.start_with?(rule) && !ASKERS.include?(x.name) }
    break if !i || t.kids[i..].any? { |x| ASKERS.include?(x.name) }
    t = t.kids[i]
  end
  t
end
# the class rules of one walk: what cf_node called for the node in any round, each path once
merge = lambda do |into, tree|
  shown[tree].each do |k|
    merge.call(into.kids.find { |x| x.name == k.name } || Tree.new(k.name, [], k.note).tap { |x| into.kids << x }, k)
  end
end
# "0-2, 4" for [0, 1, 2, 4]
ranges = ->(ns) { ns.uniq.sort.slice_when { |x, y| y != x + 1 }.map { |r| r.size > 1 ? "#{r[0]}-#{r[-1]}" : r[0].to_s }.join(", ") }
first = true
made = ->(passes) { passes.map { |p, rs| rs.compact.empty? ? p : "#{p} round#{'s' if rs.uniq.size > 1} #{ranges[rs.compact]}" } }
nodes.select { |_, n| n[3] }.sort_by { |id, n| [n[1], id] }.each do |id, (b, c, label, _)|
  items = [] # [head, body lines, the passes and rounds it was made in], in order; one that repeats the one before joins it
  rules = {} # a class walk's pass => [the merged tree of its rounds, the rounds]
  sets = {} # a walk => the set it left
  events.each do |e|
    mine = e.facts.select { |f| f[0] == id }
    next unless e.node == id || mine.any?
    fact = FACTS.fetch(e.kind)[0]
    next sets[e.pass.split.last] = mine[0][1][2..] if e.kind == "k".ord && !e.tree
    if e.kind == "k".ord
      merge.call((rules[e.pass] ||= [Tree.new(e.tree.name, [], []), []])[0], e.tree)
      next rules[e.pass][1] << e.round
    end
    next if mine.empty? # asked, and nothing of this node changed
    head = mine.map { |f| "#{FACTS.fetch(f[2])[0]}: #{f[1][2..]}" }.join("; ")
    from = e.callers - ["comp_set_ntype"]
    body = ["    from #{'... > ' if from.size > 6}#{from.last(6).join(' > ')}"]
    if !e.tree
      # (a pass's own write)
    elsif e.node == id
      body << "    #{e.tree.name}"
      render.call(e.tree, 3, last_rule[e], "last rule called", body)
    else
      on = nodes[e.node]
      body << "    inside the #{fact} of #{on[0]}:#{on[1] + 1} #{on[2]}"
    end
    rounds = e.kind == "n".ord || e.callers.include?("an_phase_infer_fixpoint")
    items << [head, body, {}] unless items.last && items.last[0, 2] == [head, body]
    (items.last[2][e.pass] ||= []) << (rounds ? e.round : nil)
  end
  puts unless first
  first = false
  puts "#{prog}:#{b}:#{c + 1}  #{label || "node #{id}"}  `#{text_at[b, c]}`"
  if sets.any?
    same = sets.values.uniq.size == 1
    puts "  class set: #{same ? "#{sets.values[0]}  (#{sets.keys.join(', ')})" : sets.map { |w, s| "#{w} #{s}" }.join("; ")}"
  end
  walks = {} # the class walks come first, as they ran; walks with the same tree share it
  rules.each do |pass, (tree, rounds)|
    render.call(tree, 3, nil, nil, body = ["    #{tree.name}"])
    (walks[body] ||= {})[pass] = rounds
  end
  seen = {} # a tree already printed => the pass it was printed under
  (walks.map { |body, passes| ["class set rules", body, passes] } + items).each do |head, body, passes|
    at = made[passes] == [""] ? "outside a pass" : "pass #{made[passes].join(', ')}"
    puts "  #{head}", "    #{at}"
    next puts body if body.size < 3 # (no tree)
    puts seen[body] ? [body[0], "    the tree printed above under #{seen[body]}"] : body
    seen[body] ||= at
  end
end
exit 1 unless ok # a failed compile is no answer, though what it was doing is printed
