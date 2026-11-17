# A BasicObject subclass answers a reader call with NoMethodError until
# the class's attr_reader runs; the reader answers after it. The call's
# arguments run before the NoMethodError, as CRuby's do before the dispatch,
# in order, whatever their shape (a container, a nil, a splat, a block-pass).

class Slate < BasicObject
  def initialize = @label = :set
end

s = Slate.new
begin
  p s.label
rescue NoMethodError => e
  puts "before: #{e.class}"
end
r = begin
  [s.label, s.label]
rescue NoMethodError => e
  "both: #{e.class}"
end
p r
m = nil
m = Slate.new if ARGV.empty?
begin
  p m.label
rescue NoMethodError => e
  puts "maybe nil: #{e.class}"
end
begin
  s.label(::Kernel.puts("arg"))
rescue NoMethodError => e
  puts "with an argument: #{e.class}"
end
begin
  s.nope(::Kernel.puts("a1"), ::Kernel.puts("a2"))
rescue NoMethodError => e
  puts "never: #{e.class}"
end
def note(t) = (puts t; t)
pr = ::Kernel.proc { |x| x }
[
  -> { s.nope(::Kernel.puts("c1"), [::Kernel.puts("c2")]) },
  -> { s.nope(nil, { k: note("n1") }, note("n2")) },
  -> { s.nope(*[note("s1"), note("s2")], &(note("b1"); pr)) },
  -> { s.nope(note("y1"), &:to_s) },
  -> { s.nope(*nil, "#{note("i1")}") },
  -> { s.nope(note("t1"), &1) },
  -> { x = s.nope([note("w1")]) },
  -> { [s.nope(note("l1")), note("l2")] },
].each do |f|
  f.call
rescue NoMethodError, TypeError => e
  puts "shapes: #{e.class}"
end

class Slate
  attr_reader :label
end

p s.label
p [s.label, s.label]
p m.label
p Slate.new.label
