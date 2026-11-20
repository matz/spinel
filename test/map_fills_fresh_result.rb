# map and map.with_index build an Integer or Float result at the receiver's
# length and store each value into its slot: a receiver the block grows
# appends past that length, one it shrinks cuts the result to the values
# made, and a break, next or raise leaves as before. Object and boxed results
# append as before and keep their elements across a collection the block's
# allocations start.
class Pt
  attr_reader :v
  def initialize(v) = (@v = v)
end

a = [1.5, 2.5]
r = a.map { |x| a << x + 10 if a.size < 5; x * 2 }
p r, a
b = [1, 2, 3, 4, 5]
r = b.map { |x| b.pop; x * 3 }
p r, b
p([1.0, 2.0, 3.0].map { |x| break 9 if x > 1.5; x })
p([1, 2, 3, 4].map { |x| next 0 if x.even?; x * 10 })
p((begin; [1.0, 2.0, 3.0].map { |x| raise "no" if x > 2; x }; rescue => e; e.message; end))
objs = (1..300).to_a.map { |x| Pt.new(x * 2) }
GC.start
p objs.size, objs[0].v, objs[299].v, objs.sum(&:v)
strs = (1..50).to_a.map { |x| "s#{x}" }
p strs[0], strs[49], strs.size
p [1, 2, 3].map { |x| [x, x * 2] }, [1, 2, 3].map { |x| x.even? ? "e" : x }
p [1, 2, 3].map { |x| x == 2 ? nil : x }
p (1..5).map { |x| x * x }, (0...4).map { |i| i * 1.5 }
p [[1, 2], [3, 4]].map { |row| row.map { |x| x * 10 } }
c = [0, 1, 2, 3, 4]
c.shift
p c.map { |x| x * 2 }
f = [1, 2, 3].freeze
p f.map { |x| x + 1 }, f.frozen?
g = [1, 2].map { |x| x }
g << 3
p g, g.frozen?
w = [1.5, 2.5, 3.5]
p w.map.with_index { |x, i| x * i }, w.map.with_index(1) { |x, i| x * i }
ws = (1..100).to_a.map.with_index { |x, i| Pt.new(x + i) }
GC.start
p ws.sum(&:v)
h = [1, 2]
p h.map.with_index { |x, i| h << 9 if h.size < 4; x + i }, h
s = [1, 2, 3, 4]
p s.map.with_index { |x, i| s.pop; x * i }, s
m = [1, 2, 3]
m.map!.with_index { |x, i| x * 10 + i }
p m
