# map's result is sized for the receiver's elements up front (as CRuby's is)
# rather than grown from 16: Integer, Float, object, row and mixed results,
# over Arrays of each size around the first capacity, a Range, and a
# receiver held boxed (an Array, a Hash). The result is an ordinary Array
# afterwards, a block that pushes onto or pops the receiver still decides
# how many values there are, and a break leaves the map at once.
# spinel: gc-stress

P = Struct.new(:v)
[0, 1, 15, 16, 17, 33, 1000].each do |n|
  a = Array.new(n) { |i| i }
  ri = a.map { |x| x * 3 }
  rf = a.map { |x| x * 0.5 }
  ro = a.map { |x| P.new(x) }
  rr = a.map { |x| [x, x + 1] }
  rm = a.map { |x| x.even? ? x : x.to_s }
  p [n, ri.size, ri.sum, rf.size, rf.sum, ro.size, ro.sum(&:v), rr.size, rr.sum(&:last), rm.size, rm.count(String)]
  ri << 7
  rf.push(1.5)
  ro.unshift(P.new(-1))
  p [ri.size, ri.last, rf.size, rf.last, ro.size, ro.first.v]
end

p (0...20).map { |i| i * i }, (0...0).map { |i| i }, (1..3).map { |i| i.to_f }

def boxed(x) = x.size > 1000 ? "s" : x
p boxed([1, "a", 2.5]).map { |v| v.to_s }, boxed({a: 1, b: 2}).map { |k, v| [k, v * 2] }
p boxed([1, 2.5, 3]).map { |v| v.to_i * 2 }, boxed(Array.new(40) { |i| i }).map { |v| v * 0.5 }.sum, boxed({a: 1, b: 2}).map { |k, v| v + 1 }

g = [1, 2]
p g.map { |x| g << x + 10 if g.size < 4; x }
h = [1, 2, 3, 4]
p h.map { |x| h.pop if x == 1; x }
p [1, 2, 3].map { |x| break x * 100 if x == 2; x }
m = [1, 2, 3]
m.map! { |x| x * 2 }
p m
