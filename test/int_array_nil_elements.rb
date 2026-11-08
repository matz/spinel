# spinel: gc-stress
# An Integer or Float array holds nil as its slot's sentinel (SP_INT_NIL, or
# the Float NaN payload) once a nilable value is stored in it, and inspect and
# compact already read it as nil. The searches folded a nil needle to a miss,
# join and puts printed the sentinel's number, sum added it and min, max and
# sort ordered it where CRuby raises, and a copy into a poly array (a set
# operation, ==, a zip row, a splat) boxed it as a number. Array#<=> asks each
# pair `<=>`, where nil against nil is 0; a mutation through a reader, an
# alias, a method or a subclass marks the ivar it lands in; unshift, insert and
# fill store a nilable Integer; and pack's float directives refuse a poly nil.
# The marking follows a conditional's arms, and an index write past the end
# leaves nil in the gap. Behind a poly handle (`y || [b, 3]`, `c && [..]`, a
# branch mixing Integer and Float arrays) the runtime reads the sentinel as
# nil, and minmax takes a leading nil as an element.
def try
  yield
rescue => e
  e.class
end

class R1; attr_reader :a; def initialize = (@a = [1, 2]); def get = @a; end
class R2; attr_reader :a; alias vals a; def initialize = (@a = [1, 2]); def get = @a; end
class R3; def initialize = (@a = [1, 2]); def a = @a; def get = @a; end
class R4; attr_reader :a; def initialize = (@a = [1, 2]); def get = @a; end
class R5 < R4; end
class R6; attr_reader :a; def initialize = (@a = [0.5]); def get = @a; end
class R7; def initialize = (@a = [1, 2]); def get = @a; end
class R8 < R7; def add(v) = (@a << v); end

class Box
  def initialize(v) = (@v = [v, 9])
  attr_reader :v
  alias vals v
end

ints = [5, 6]
flts = [1.5, 2.5]
i = ARGV.size + 7
b = ints[i]
f = flts[i]
x = [b, 3]
y = [4.5]
y << f
p x.include?(nil), x.index(nil), x.rindex(nil), x.count(nil)
p y.include?(nil), y.index(nil), y.count(nil), Box.new(b).v.count(nil)
vs = Box.new(b).vals
p vs.include?(nil), vs.index(nil), try { vs.sum }
p x.join("-"), y.join("-"), x * ",", "%s|%p" % y
puts x, y
p try { x.sum }, try { y.sum }, try { x.sum(0.5) }, try { x.max }, try { y.min }
p try { x.sort }, try { y.minmax }, [b, b].sort, [f].max
p x == [nil, 3], y == [4.5, nil], y == [4.5, f], x.eql?([nil, 3]), x.hash == [nil, 3].hash
p x - [nil], y & [nil], x | [nil], y.intersect?([nil])
p x.zip([1])[0][0].nil?, [1, 2, 3].zip(y)[2][1].nil?, y.product([0]), [*y, "s"][1].nil?
p((x + [nil])[0].nil?, [x].flatten[0].nil?, x.all?, y.none?, x.one?, x.any?(nil))
p try { x.pack("q*") }, try { y.pack("d*") }
n = [nil, "s"][ARGV.size]
p x.include?(b), [7].include?(b), y.include?(f), x.index(n), y.include?(n)
z = [b, 8]
p z.delete(nil), z, [f, 2.5].compact!
p([b, 3] <=> [b, 3], [f, 2.5] <=> [f, 2.5], [nil, 3] <=> [nil, 3], x <=> [nil, 4], [true] <=> [true])
p [[nil, 1], [b, 0]].sort, [x] <=> [[nil, 3]], [b] <=> [1]
r1, r2, r3, r5, r6, r8 = R1.new, R2.new, R3.new, R5.new, R6.new, R8.new
r1.a << b; r2.vals.push(b); r3.a[3] = b; r5.a.insert(0, b); r6.a.concat([f]); r8.add(b)
p r1.get.include?(nil), r2.get.index(nil), r3.get.count(nil), r5.get.index(nil), r6.get.include?(nil), r8.get.include?(nil)
a2 = [1, 2]
a2.unshift(b); a2.insert(2, b); p a2, a2.count(nil), [1, 2].fill(b).index(nil)
a3 = [1]
(a3 << 2) << b; p a3.include?(nil), [1].push(b).include?(nil)
p try { [nil, 3.5].pack("d*") }, try { [1.5, n].pack("e2") }, [1, n].pack("qx")
c = ARGV.size == 0
p try { (c ? [3, b] : [3, b]).max }, (c ? [3, b] : [4]).include?(nil)
c1 = if c then [f, 1.5] else [2.5] end
c2 = case ARGV.size when 0 then [3, b] else [4] end
c3 = begin; raise "e"; rescue; [b, 2]; end
p c1.index(nil), c2.count(nil), c3.join("-")
g1 = [1, 2]
g1[4] = 5
g2 = [1.5]
g2[g2.size + 1] = 2.5
g3 = [1, 2]
g3[2] = 3
p g1.index(nil), g1.count(nil), g2.include?(nil), g3.include?(nil)
y0 = [nil, 1][ARGV.size]
h1 = y0 || [b, 3]
h2 = c && [2.5, f]
h3 = c ? [f] : [4]
p h1.include?(nil), h2.include?(n), h3.include?(nil), try { h1.sum }, try { h2.sum }, h3.max
m1 = begin; h1.minmax; rescue ArgumentError => e; e.class; end
m2 = begin; (y0 || [nil, 3]).minmax; rescue ArgumentError => e; e.class; end
p try { h1.max }, try { h2.min }, m1, m2, h3.minmax
h1.delete(nil)
h2.delete(nil)
p h1, h2
