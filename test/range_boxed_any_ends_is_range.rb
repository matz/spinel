# A Range is a Range whatever its ends are. One that reaches a class test
# through a boxed value (a parameter several callers type, a Hash value, an
# Array element, an instance variable) carries the class id of its
# representation, which the ends choose: Integers, Floats, Strings, Times,
# the program's objects.
require "date"

class Ver
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = Ver.new(n + 1)
  def inspect = "#<Ver #{n}>"
end

class Holder
  def initialize(v) = @v = v
  def kind
    case @v
    when Range then "range"
    else "other"
    end
  end
end

def ends_of(v)
  out = Array.new(0) { "" }
  case v
  when Range
    out << v.begin.inspect
    out << v.end.inspect
    out << v.exclude_end?.to_s
  end
  out
end

def kind(v)
  case v
  when Integer, Float then "number"
  when Range then "range"
  when Enumerable then "enumerable"
  else "other"
  end
end

def pattern(v)
  case v
  in Range => r then "range #{r.exclude_end?}"
  in Integer | Float then "number"
  else "other"
  end
end

def optional(r = nil)
  case r
  when Range then "range #{r.begin.inspect}"
  when nil then "nil"
  else "other"
  end
end

Pair = Struct.new(:r, :n)

def tests(v) = [v.is_a?(Range), v.kind_of?(Range), v.instance_of?(Range), Range === v, v.class == Range]
def mixins(v) = [v.is_a?(Enumerable), Enumerable === v, v.is_a?(Comparable)]
def is_a_of(v, k) = [v.is_a?(k), v.kind_of?(k), v.instance_of?(k), k === v]
def covers(r, x) = [r.cover?(x), r.include?(x), r === x]

t1 = Time.utc(2026, 1, 1)
t2 = Time.utc(2026, 2, 1)
tm = Time.utc(2026, 1, 15)
d1 = Date.new(2026, 1, 1)
d2 = Date.new(2026, 2, 1)
v1 = Ver.new(1)
v3 = Ver.new(3)

puts "-- the ends, through `when Range`"
p ends_of(1.5..2.5)
p ends_of("a".."z")
p ends_of(1..2)
p ends_of(t1..t2)
p ends_of(v1...v3)
p ends_of(2.5)
p ends_of(t1)

ranges = [1..2, 1...2, 1.5..2.5, 1.5...2.5, Range.new(1.5, 2.5), Range.new(1.5, 2.5, true),
          (1.5..), (..2.5), 1..2.5, 1.5..3, "a".."z", "a"..."z",
          t1..t2, t1...t2, Range.new(t1, t2), Range.new(t1, t2, true), (t1..), (..t2),
          d1..d2, d1...d2, Range.new(d1, d2), (d1..), (..d2),
          v1..v3, v1...v3, Range.new(v1, v3), (v1..), (..v3), nil..nil]
others = [1, 2.5, "a", :a, nil, true, t1, d1, v1, [1.5, 2.5], { a: 1.5 }]

puts "-- an Array element"
ranges.each_with_index { |r, i| puts "#{i}: #{kind(r)} / #{pattern(r)} / #{tests(r)} / #{mixins(r)}" }
others.each_with_index { |o, i| puts "#{i}: #{kind(o)} / #{pattern(o)} / #{tests(o)}" }

puts "-- a class held in a value"
ranges.each_with_index { |r, i| puts "#{i}: #{is_a_of(r, Range)} #{is_a_of(r, Enumerable)} #{is_a_of(r, Float)}" }
others.each_with_index { |o, i| puts "#{i}: #{is_a_of(o, Range)}" }
p ranges.grep(Range).size, others.grep(Range).size
p ranges.count { |r| Range === r }, (ranges + others).select { |x| x.is_a?(Range) }.size

puts "-- a Hash value"
h = { int: 1..2, float: 1.5..2.5, str: "a".."z", time: t1..t2, date: d1...d2, ver: v1..v3, none: 7 }
h.each { |k, r| puts "#{k}: #{kind(r)} #{pattern(r)} #{ends_of(r).size}" }
p ends_of(h[:time]).size, ends_of(h[:float]).size, ends_of(h[:none]).size

puts "-- an instance variable"
p [1.5..2.5, t1..t2, d1..d2, v1..v3, "a".."z", 1..2, 2.5, t1].map { |x| Holder.new(x).kind }

puts "-- a Struct member, a parameter with a nil default"
[Pair.new(1.5..2.5, 1), Pair.new(t1..t2, 2), Pair.new(v1..v3, 3), Pair.new(2.5, 4)].each do |x|
  case x.r
  when Range then puts "#{x.n}: range #{x.r.end.inspect}"
  else puts "#{x.n}: other"
  end
end
p optional, optional(1.5..2.5), optional(t1..t2), optional(d1..d2), optional(nil)

puts "-- inside a pattern, and as a pattern for the elements"
mixed = [1.5..2.5, t1..t2, 3, nil]
case mixed
in [Range => x, Range => y, Integer, nil] then puts "array #{x.begin} #{y.exclude_end?}"
else puts "no"
end
case { r: t1..t2 }
in { r: Range => r } then puts "hash #{r.begin.year}"
else puts "no"
end
case mixed[0]
in Range | Array then puts "alternative"
else puts "no"
end
p mixed.grep_v(Range).size, mixed.any?(Range), mixed.all?(Range), mixed.none?(Range)
p mixed.partition { |v| v.is_a?(Range) }.map(&:size), mixed.find { |v| Range === v }.class

puts "-- a value of the Range's end type"
p covers(h[:float], 2.0), covers(h[:float], 2.5), covers(h[:float], 3.0)
p covers(h[:time], tm), covers(h[:time], t2), covers(h[:time], Time.utc(2027, 1, 1))
p covers(h[:date], Date.new(2026, 1, 15)), covers(h[:date], d2)
p covers(h[:ver], Ver.new(2)), covers(h[:ver], Ver.new(4))
p covers(h[:str], "m"), covers(h[:int], 2)
[1.5...2.5, (1.5..), (..2.5), t1...t2, (t1..), (..t2)].each do |r|
  x = r.begin || r.end
  puts "#{r.begin.inspect} #{r.end.inspect} #{r.exclude_end?} #{covers(r, x)}"
end

puts "-- a slot that holds only one kind of Range"
fr = 1.5..2.5
tr = t1..t2
p fr.is_a?(Range), Range === fr, tr.is_a?(Range), Range === tr, tr.is_a?(Enumerable)
case fr
when Range then puts "range"
else puts "other"
end
case tr
when Range then puts "range"
else puts "other"
end
