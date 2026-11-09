# Holds spinel's class facts (#8530) against a CRuby run of the same
# program. The facts come from `SPINEL_CLS_FACTS=1 spinel ... -c`, the
# observations from tools/cls_fact_trace.rb. A fact is a may-hold set: a
# receiver class CRuby saw at a call site must be in the site's set, or a
# consumer may drop the arm that call needs (#8720).
#
#   ruby tools/cls_fact_check.rb FACTS TRACE
#
# Prints one line per violation and exits 1 when there is any. A receiver
# whose class the program defines must be in the site's set; a receiver of a
# builtin class (a call of a method CRuby defines in C) must be in the set's
# builtin part, +builtin[Integer String ...]: its class, `other` for a class
# the core table lacks, or `any`. nil is not checked (the nil fact's). A
# `send/m` fact is a send of a literal name: the receiver of the call of m
# CRuby makes. Several calls of one name on one line are held against the
# union of their sets. A builtin receiver at an empty fact (no value reaches
# the call, for the analysis) is no violation, as the readers take such a
# fact as unknown (CLS_FACT_EMPTIES=1 prints them), and a builtin receiver
# of an operator on a line that assigns with it (`x += 1`) is not checked:
# no call node of the facts states it. A builtin class the program reopens is a
# class of its set that stands for that class and its builtin subclasses, either
# way up. A call of a builtin CRuby writes in Ruby (`<internal:...>`, Integer#size)
# on a builtin class is a builtin call. A call on a class of the program's that
# reached one of those is marked [builtin]: no dispatch
# arm is lost there, but the fact is still wrong. A call a C method of
# CRuby makes back into the program is not the line's call: it is left
# out (printed to stderr with CLS_FACT_CALLBACKS=1). A line with a send
# (`send`, `public_send`, `__send__`) is skipped for a receiver of a class of
# the program's: the method it reaches is not the call the fact is about.
require "set"

facts_path, trace_path = ARGV
facts = Hash.new { |h, k| h[k] = Set.new }
builtin = Hash.new { |h, k| h[k] = Set.new }
files = Set.new
sends = Set.new
File.foreach(facts_path, encoding: "UTF-8") do |l|
  l = l.scrub
  next unless l.start_with?("cls-fact: ")
  m = l.chomp.match(/\Acls-fact: (.*):(\d+) (\S+) receiver:(.*)\z/) or next
  path = File.expand_path(m[1])
  files << path
  meth, lit = m[3].split("/", 2)
  # the builtin part: +builtin, then [the classes bk holds] when it names any
  # (a send is held under the name it reaches, and under its own: CRuby runs public_send as a C method, whose
  # inner call is not recorded)
  is_send = %w[send public_send __send__].include?(meth)
  keys = [[path, m[2].to_i, lit || meth]]
  keys << [path, m[2].to_i, meth] if is_send && lit
  bk = []
  recv = m[4].sub(/\+builtin(?:\[([^\]]*)\])?/) { bk = $1.to_s.split(" "); "" }
  keys.each { |k| builtin[k].merge(bk); facts[k].merge(recv.split(" ")) }
  sends << [path, m[2].to_i] if is_send
end

bad = []
callbacks = []
empties = []
source = Hash.new { |h, p| h[p] = (File.readlines(p, encoding: "UTF-8").map(&:scrub) rescue []) }
# a class defined by its path (`class A::B::C`) is named by its leaf
# (or by a name qualify_colliding_classes joined with __)
leaf = ->(n) { n.gsub("__", "::").sub(/\AClass\((.*)\)\z/) { "Class(#{$1.split('::').last})" }.split("::").last }
# CRuby's class of a name, nil when this Ruby has none
klass = ->(n) { (k = Object.const_get(n)).is_a?(Module) ? k : nil rescue nil }
# May a receiver of builtin class recv be at a fact: in the builtin part (its class, `other` for a class the core
# table lacks, `any`); a Class or a Module where the set holds a Class object; or one of the set's classes that is
# a builtin class the program reopens, which stands for that class and its builtin subclasses, either way up
builtin_ok = lambda do |recv, set, b|
  next true if recv == "NilClass" || set.include?("ANY") || b.include?("any") || b.include?("other") || b.include?(recv)
  next true if %w[Class Module].include?(recv) && set.any? { |e| e.start_with?("Class(") }
  rk = klass.(recv)
  set.any? do |e|
    next false if e == "none" || e.start_with?("Class(")
    mk = klass.(e)
    # (a class of a package spinel has as a class of the program's under another name: IO::Buffer, Date)
    mk && rk ? (rk <= mk || mk <= rk) : leaf.(e) == leaf.(recv)
  end
end
File.foreach(trace_path, encoding: "UTF-8") do |l|
  path, line, meth, recv, src, callee, root = l.scrub.chomp.split("\t", -1)
  set = facts[[path, line.to_i, meth]] if facts.key?([path, line.to_i, meth])
  next unless set
  # a call of a C method a class of the program's inherits from a builtin class (the seventh field): its receiver is in
  # the class part, or the fact says it may be the builtin class (a builtin call the facts do not model hands back the
  # program's subclasses of it)
  if callee == "c_call" && src.to_s.empty? && !root.to_s.empty?
    b = builtin[[path, line.to_i, meth]]
    next if set.include?("ANY") || set.any? { |e| leaf.(e) == leaf.(recv) } || builtin_ok.(root, set, b)
    if set.to_a == ["none"] && b.empty?
      empties << "#{path}:#{line} #{meth}: receiver #{recv} (a #{root}) at an empty fact"
      next
    end
    bad << "#{path}:#{line} #{meth}: receiver #{recv} (a #{root}) not in {#{set.to_a.sort.join(' ')}} +builtin[#{b.to_a.sort.join(' ')}]"
    next
  end
  # a call of a C method, or of a builtin CRuby writes in Ruby (`<internal:...>`: Integer#size), on a builtin class
  if callee == "c_call" || (callee.to_s.start_with?("<internal:") && src.to_s.empty?)
    # an operator assignment (`x += 1`, `h[k] += 1`) calls its operator, which is no call node of the facts
    next if meth.match?(/\A\W+\z/) && source[path][line.to_i - 1].to_s.match?(/(?:[-+*\/%|&^]|<<|>>|\*\*|\|\||&&)=/)
    b = builtin[[path, line.to_i, meth]]
    rc = recv.start_with?("Class(") ? "Class" : recv
    next if builtin_ok.(rc, set, b)
    # a fact that is empty (no value reaches the call, for the analysis) rules nothing out: its readers take it as unknown
    if set.to_a == ["none"] && b.empty?
      empties << "#{path}:#{line} #{meth}: builtin receiver #{recv} at an empty fact"
      next
    end
    bad << "#{path}:#{line} #{meth}: builtin receiver #{recv} not in +builtin[#{b.to_a.sort.join(' ')}]"
    next
  end
  next if sends.include?([path, line.to_i])
  next if set.include?("ANY") || set.include?(recv)
  next if set.any? { |e| leaf.(e) == leaf.(recv) }
  # a Class.new spinel names SpinelAnonClassN, which CRuby names by the
  # constant it is later assigned to
  next if set.any? { |e| e.include?("SpinelAnonClass") }
  next if src.nil? || src.empty? || !files.include?(File.expand_path(src))
  # a C method's call back into the program: reported, not counted
  if callee.to_s.start_with?("callback:")
    callbacks << "#{path}:#{line} #{meth}: receiver #{recv} not in {#{set.to_a.sort.join(' ')}} [callback]"
    next
  end
  tag = callee.to_s.start_with?("<internal:") ? " [builtin]" : ""
  bad << "#{path}:#{line} #{meth}: receiver #{recv} not in {#{set.to_a.sort.join(' ')}}#{tag}"
end
bad.uniq.each { |b| puts b }
callbacks.uniq.each { |b| warn b } if ENV["CLS_FACT_CALLBACKS"]
empties.uniq.each { |b| warn b } if ENV["CLS_FACT_EMPTIES"]
exit(bad.empty? ? 0 : 1)
