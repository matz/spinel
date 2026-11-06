# spinel: gc-stress
# A <=> whose operand is not its first parameter (an optional comes ahead of
# it) takes the operand where the argument layout binds it: the runtime cmp
# dispatch guards and converts it by that parameter's type, and the passes
# that box the operand of a shared <=> or of a sort key's <=> widen that one,
# through sort, min, max, Comparable, sort_by and min_by, typed and boxed.
# A rest that takes the operand (`def <=>(*r)`, `def <=>(*r, o)`), shared by
# two subclasses, is dispatched on the boxed operand the rest packs.
# (No forwarded proc: its keys box every <=> operand of the program, which
# hides the shapes.)
class V
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(scale = 1, other) = (n <=> other.n) * scale
  def inspect = "V#{n}"
end
class C
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(scale = 1, other) = (n <=> other.n) * scale   # nothing but Comparable and sort call it
  def inspect = "C#{n}"
end
class W
  def k = 1
end
class T
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(scale = W.new, other) = (n <=> other.n) * scale.k   # the leading optional is an object
  def inspect = "T#{n}"
end
class NilSelf
  def <=>(s = 1, o) = nil   # named with an Integer below, nil for its own kind
end
class Shape
  include Comparable
  attr_reader :a
  def initialize(a) = @a = a
  def <=>(s = 1, o) = (a <=> o.a) * s   # shared by two subclasses
end
class Sq < Shape; end
class Ci < Shape; end
class RBase
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(*r) = n <=> r[0].n   # the rest takes the operand
end
class RA < RBase; end
class RB < RBase; end
class PBase
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(*r, o) = (n <=> o.n) + r.size * 0   # a required parameter after the rest
end
class PA < PBase; end
class PB < PBase; end
def t
  yield
rescue StandardError => e
  puts "#{e.class}: #{e.message}"
end

w = ARGV.size > 5 ? 1 : [1, 3, 2]
p [1, 3, 2].sort_by { |x| V.new(x) }, w.sort_by { |x| V.new(x) }
p [1, 3, 2].min_by { |x| V.new(x) }, w.max_by { |x| V.new(x) }
vs = [V.new(2), V.new(1), V.new(3)]
bv = ARGV.size > 5 ? 1 : vs
p vs.sort, vs.min, vs.max, bv.sort, bv.max

a = C.new(1); b = C.new(2)
x = ARGV.size > 5 ? 1 : a
p a < b, x < b, [b, a].sort, [b, a].max, x.clamp(a, b)

p(T.new(1) <=> T.new(2))
ts = ARGV.size > 5 ? 1 : [T.new(2), T.new(1), T.new(3)]
p [T.new(2), T.new(1)].sort, ts.sort, ts.max, [1, 3, 2].sort_by { |x| T.new(x) }

p(NilSelf.new <=> 1)
ns = NilSelf.new
t { p [0, 1].sort_by { |i| ns } }
t { p w.sort_by { |i| ns } }

s = Sq.new(2); c = Ci.new(1)
sh = ARGV.size > 5 ? 1 : [s, c, Ci.new(0)]
p s > c, [s, c, Sq.new(3)].sort.map(&:a), [s, c].max.a, sh.sort.map(&:a), sh.min.a

rs = [RA.new(3), RB.new(1), RA.new(2)]
p rs.sort.map(&:n), rs.min.n, rs.max.n, rs.minmax.map(&:n)
p RB.new(2).clamp(RB.new(1), RA.new(3)).n, RA.new(1) < RB.new(2), RA.new(1) == RB.new(1)
p rs.sort_by { |x| x }.map(&:n), rs.max_by { |x| x }.n
brs = ARGV.size > 5 ? 1 : rs
p brs.sort.map(&:n), brs.min.n
ps = [PA.new(3), PB.new(1), PA.new(2)]
p ps.sort.map(&:n), ps.min.n, ps.max.n, PA.new(1) <=> PB.new(2), PB.new(3) > PA.new(2)
