# A BasicObject subclass answers a reader call with NoMethodError until
# the class's attr_reader runs; the reader answers after it.

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

class Slate
  attr_reader :label
end

p s.label
p [s.label, s.label]
p m.label
p Slate.new.label
