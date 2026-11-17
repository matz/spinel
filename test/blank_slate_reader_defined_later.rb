# A BasicObject subclass answers a reader call with NoMethodError until
# the class's attr_reader runs; the reader answers after it. The call's
# arguments run before the NoMethodError, as CRuby's do before the dispatch.

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

class Slate
  attr_reader :label
end

p s.label
p [s.label, s.label]
p m.label
p Slate.new.label
