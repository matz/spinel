# spinel: gc-stress
# `map` over a receiver held boxed reads an Array's length at each step, as
# the typed walks and CRuby do: a block that pushes onto the receiver maps
# what it added, and one that pops it stops at the new end. A Hash, a Range
# and a user Enumerable map as before.

def boxed(x) = x.respond_to?(:zzz) ? 0 : x

class Pt
  attr_reader :v
  def initialize(v) = @v = v
end

class Bag
  include Enumerable
  def initialize(*xs) = @xs = xs
  def each(&b) = @xs.each(&b)
end

ints = boxed([1, 2])
p ints.map { |v| ints << v + 10 if ints.size < 4; v * 2 }
shrink = boxed([1, 2, 3, 4])
p shrink.map { |v| shrink.pop if v == 1; v }

floats = boxed([1.5, 2.5])
p floats.map { |v| floats << v + 10 if floats.size < 3; v }
floats2 = boxed([1.5, 2.5, 3.5])
p floats2.map { |v| floats2.shift if v == 1.5; v }

strs = boxed(["a", "b"])
p strs.map { |s| strs << s + "!" if strs.size < 4; s.upcase }

objs = boxed([Pt.new(1), Pt.new(2), Pt.new(3)])
p objs.map { |o| objs.pop if o.v == 1; o.v }

mixed = boxed([1, "two", 3.0])
p mixed.map { |x| mixed << :four if mixed.size < 4; x.to_s }

emptied = boxed([1, 2, 3])
p emptied.map { |v| emptied.clear; v }

h = boxed({ a: 1, b: 2 })
p h.map { |k, v| "#{k}=#{v}" }
p boxed(1..4).map { |v| v * v }
p boxed(Bag.new(3, 4)).map { |v| v + 1 }
