# `sum { }` over an Array of boxed values (objects, or a mix) adds each
# block value to a running total as CRuby does, rather than mapping the
# whole array first: the block stops being called at the element whose
# value cannot be added, and the answer keeps CRuby's class (Integer 0 for
# an empty receiver, a compensated Float sum once a Float is added).

P = Struct.new(:a, :b)
def pa(n) = Array.new(n) { |i| P.new(i, i * 0.5) }
xs = pa(4)

# the block's forms: next, it, a numbered parameter, a forwarded block, a
# Symbol's proc
p xs.sum { |q| next 0 if q.a == 1; q.a }
p xs.sum { it.b }, xs.sum { _1.a * 2 }
def fwd(xs, &blk) = xs.sum(&blk)
p fwd(xs) { |q| q.b * 2 }
p xs.sum(&:a), xs.sum(&:b)

# empty, one element, and the answer's class
p pa(0).sum(&:b), pa(1).sum(&:b), pa(0).sum { |q| q.b }.class, pa(2).sum(&:b).class

# Integers and Floats from the block, in any order
k = 0
p xs.sum { |q| k += 1; k.odd? ? q.a : q.b }
p [1, 2.5, P.new(3, 0)].sum { |v| v.is_a?(P) ? v.a : v }

# compensated: the Float total CRuby answers
p Array.new(10) { P.new(0, 0.1) }.sum(&:b)
p [P.new(0, 1e100), P.new(0, 1.0), P.new(0, -1e100)].sum(&:b)
p [P.new(0, 1.0), P.new(0, Float::INFINITY)].sum(&:b), [P.new(0, 1.0), P.new(0, Float::NAN)].sum(&:b).nan?

# pairs splat into two parameters
p [[1, 2.0], [3, 4.0]].sum { |x, y| x * y }

# a value that cannot be added raises there, and the block is not called
# for the elements after it
mk = [P.new(1, 2.5), P.new(3, 4.5), "x", 7, nil, [1, 2]]
seen = []
p((mk.sum { |v| seen << v.class; v.is_a?(P) ? v.a : v } rescue $!.class), seen)
seen = []
p((mk.sum { |v| seen << v; v.is_a?(P) ? v.b : nil } rescue $!.class), seen.size)
p((xs.sum { |q| q.a.to_s } rescue $!.class), (xs.sum { |q| [q.a] } rescue $!.class))
c = 0
p((xs.sum { |q| c += 1; raise "boom" if q.a == 2; q.a } rescue $!.message), c)

# break, with a value and without
p xs.sum { |q| break q.b if q.a == 2; q.a }
p xs.sum { |q| break if q.a == 2; q.a }

# in an instance method over an instance variable
class Order
  def initialize(items) = (@items = items)
  def total = @items.sum { |it| it.a * it.b }
  def count = @items.sum(&:a)
end
o = Order.new(pa(5))
p o.total, o.count

# a narrowed receiver: a table's rows and an Array of one class, bound as
# the walks bind them, and the blocks they cannot bind that way (a row
# taken apart, a parameter that holds another class)
it2 = [[1, 2], [3, 4]]
p it2.sum { |r| r.sum }, it2.sum { |a, b| a * b }, [[1.5, 2.0], [3.0, 0.5]].sum { |a, b| a * b }
p [[1, 2, 3], [4, 5, 6]].sum { |a, *r| a + r.size }, [[1, 2, 3], [4, 5, 6]].sum { |(a, b)| a + b }
ns = [P.new(1, 0.5), P.new(2, 1.5)]
p ns.sum { |q| q.b }, ns.sum { |q| q = P.new(q.a * 10, 0) if q.a > 1; q.a }

# a block that grows or shrinks the receiver: each step reads its length,
# as CRuby's does (boxed, of one class, and typed receivers alike)
g = [1, "a"]
p g.sum { |x| g << 1 if g.size < 3; x.is_a?(Integer) ? x : 1 }
h = [1, "a"]
p h.sum { |x| h.pop if h.size > 1 && x == 1; x.is_a?(Integer) ? x : 1 }
gs = [P.new(1, 0), P.new(2, 0)]
p gs.sum { |q| gs << P.new(5, 0) if gs.size < 3; q.a }
gi = [1, 2]
p gi.sum { |x| gi << 10 if gi.size < 3; x }
gf = [1.5]
p gf.sum { |x| gf << 2.5 if gf.size < 2; x }
# a receiver held boxed, an Array or a String
def ab(i) = i.even? ? [1, 2] : "s"
bx = ab(0)
p bx.sum { |v| bx << 10 if bx.size < 3; v }
by = ab(0)
p by.sum { |v| by.pop if v == 1; v }
bz = ab(0)
p bz.sum(0.5) { |v| bz << 2.5 if bz.size < 3; v }, ab(1).sum { |v| v }
# ... and one held boxed that is an Array of Integers or Floats, walked as
# itself rather than a copy; rows, Symbols, Strings, objects and a Hash
ti = [1, 2]
tb = ti.size > 5 ? "s" : ti
p tb.sum { |v| tb << 10 if tb.size < 3; v }
tf = [1.5]
tg = tf.size > 5 ? :q : tf
p tg.sum { |v| tg << 2.5 if tg.size < 2; v }
tp = [1, 2, 3]
tq = tp.size > 5 ? "s" : tp
p tq.sum { |v| tq.pop if v == 1; v }
tt = [[1, 2], [3, 4]]
tx = tt.size > 5 ? "s" : tt
p tx.sum { |a, b| a * b }, tx.sum { |r| r.sum }, tx.sum { |(a, b)| a - b }
sy = [:ab, :c]
sx = sy.size > 5 ? 1 : sy
st = ["ab", "cde"]
sz = st.size > 5 ? 1 : st
p sx.sum { |s| s.size }, sz.sum { |s| s.size }, sz.sum("") { |s| s * 2 }
os = [P.new(1, 1.5), P.new(2, 2)]
ox = os.size > 5 ? 1 : os
ni = [1, nil, 3]
nx = ni.size > 5 ? "s" : ni
hh = {a: 1, b: 2}
hx = hh.size > 5 ? "s" : hh
p ox.sum { |o| o.b }, ox.sum(&:b), nx.sum { |v| v || 10 }, hx.sum { |k, v| v }
# a seed that pushes onto an empty receiver runs before the walk reads its
# length, so what it pushed is summed
es = []
es << P.new(0, 0) if es.size > 3
p es.sum((es << P.new(5, 0); 0)) { |q| q.a > 2 ? q.a : 1.5 }
ei = []
ei << 1 if ei.size > 3
ef = []
ef << 1.0 if ef.size > 3
p ei.sum((ei << 7; 0)) { |v| v * 2 }, ef.sum((ef << 2.5; 0.0)) { |v| v * 2 }
# ... and the Float sum keeps its compensation, and an Integer seed over
# Floats answers the sum rather than the seed
fz = []
fz << 1.0 if fz.size > 3
fw = []
fw << 1.0 if fw.size > 3
fr = fw.sum((fw << 2.5; 0)) { |v| v * 2 }
fr = "x" if fr == 99
p fz.sum((10.times { fz << 0.1 }; 0.0)) { |v| v }, fr
p [1, "x"].sum { |*v| v.length }, [1, "x"].sum { |a, *r| r.size + 1 }
