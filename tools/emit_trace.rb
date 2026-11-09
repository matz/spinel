#!/usr/bin/env ruby
# frozen_string_literal: true

# Which emitters produced a call's C, and which arm of the call chain took it.
#
#   make build/spinel-trace
#   ruby tools/emit_trace.rb PROG.rb [LINE]
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
# - A leaf called several times in a row prints once, with (xN).
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

require "optparse"
require "tmpdir"

PREFIXES = %w[emit_ cplan_ repr_ bop_ call_plan call_target].freeze
EMITTER = "emit_"
CHAIN = %w[emit_call_body_ordinary emit_or_take_back].freeze # the mark goes to what these called
SITES = { "c".ord => ["call", "emit_call_body"], "s".ord => ["stmt", "emit_stmt"],
          "l".ord => ["tail", "emit_stmt_tail"] }.freeze # tail: the last statement of a method that answers it
HEAD, NODE, ENTER, EXIT, TEXT = 1, 2, 3, 4, 5
Tree = Struct.new(:name, :kids, :note) do
  def emits? = name.start_with?(EMITTER) || !note.nil? || kids.any?(&:emits?)
end
Frame = Struct.new(:off, :tree) # tree: where this frame's callees attach, nil outside a traced node

prefixes = PREFIXES.dup
all = false
spinel = File.expand_path("../build/spinel-trace", __dir__)
opts = OptionParser.new("usage: emit_trace.rb [options] PROG.rb [LINE]") do |o|
  o.on("--prefix P", "also show functions whose names start with P") { |p| prefixes << p }
  o.on("--all", "show every function, at every depth") { all = true }
end
opts.parse!
prog, line = ARGV
abort opts.to_s unless prog && ARGV.size <= 2
abort "emit_trace: #{spinel} is not built (make build/spinel-trace)" unless File.executable?(spinel)
line &&= Integer(line)

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

source = File.readlines(prog, chomp: true)
sections = [] # [position, label, source text, tree, emitted in], in the order the nodes were emitted
calls = {}.compare_by_identity # a call's tree => its position and label
ok = false
Dir.mktmpdir("emit_trace") do |dir|
  trace = File.join(dir, "trace")
  env = { "SPINEL_EMIT_TRACE" => trace, "SPINEL_EMIT_TRACE_LINE" => line&.to_s } # nil unsets it
  ok = system(env, spinel, prog, "-S", out: File::NULL)
  abort "emit_trace: no trace written" unless File.size?(trace)
  warn "emit_trace: the compile failed; the trace stops where it did" unless ok

  slide = nil # symbol table address minus trace offset
  label = nil # the last marker's, which its TEXT records complete
  stack = []
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
          at.kids << (at = Tree.new(name, [])) if at && (all || name.start_with?(*prefixes))
          stack << Frame.new(off, at)
        when EXIT
          i = stack.rindex { |fr| fr.off == off } # a longjmp leaves frames without their exit
          stack.slice!(i..) if i
        when TEXT
          label << [a, b, c].pack("L3").delete("\0")
        when NODE
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
          text = (b > 0 && source[b - 1] || "")[c..].to_s.strip
          text = "#{text[0, 57]}..." if text.size > 60
          sections << [where, label, text, frame.tree, arm && [arm.name, "of", *calls[path[i]][0, 2]]]
        end
      end
    end
  end
end
abort "emit_trace: nothing was emitted for #{prog}#{":#{line}" if line}" if sections.empty?

# what a function called, as shown: nothing below a helper that reaches no emitter
shown = ->(tree) { all || tree.name.start_with?(EMITTER) || tree.kids.any?(&:emits?) ? tree.kids : [] }
render = lambda do |tree, depth, took, out|
  kids = shown[tree]
  leaf = ->(k) { k && !k.note && shown[k].empty? }
  i = 0
  while i < kids.size
    k = kids[i]
    n = 1
    n += 1 while leaf[k] && leaf[kids[i + n]] && kids[i + n].name == k.name
    text = "#{'  ' * depth}#{k.name}"
    text += " (x#{n})" if n > 1
    text += "  [#{k.note.join(' ')}]" if k.note
    text = "#{text.ljust(47)} <- took the call" if kids[i, n].any? { |x| x.equal?(took) }
    out << text
    render.call(k, depth + 1, took, out)
    i += n
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
  render.call(tree, 2, took, out)
  puts if i > 0
  puts out
end
exit 1 unless ok # a failed compile is no answer, though what it emitted is printed
