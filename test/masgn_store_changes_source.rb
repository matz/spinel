# A multiple assignment spreads the whole right side Array first, then
# stores the targets left to right. A target whose store runs code (a
# writer, a user `[]=`, a Hash key's own #hash, a nested target's to_ary)
# or changes an Array in place (`a[1], b = a`) can change that Array, and
# a later target still takes the element as it was spread.

# a writer that changes an Integer Array: later targets, a splat after it,
# the assignment's own value
class IntBox
  attr_reader :x
  def x=(v)
    $ia[1] = 9
    $ia << 10
    @x = v
  end
  def go
    self.x, c = $ia
    [x, c]
  end
end
$ia = [1, 2, 3]
o = IntBox.new
o.x, c, *r = $ia
p [o.x, c, r]
$ia = [1, 2]
p IntBox.new.go
$ia = [1, 2]
r = (o.x, c = $ia)
p [r, c, r.equal?($ia)]

# the writer as the splat target, and before a middle splat
class TailBox
  attr_reader :x
  def x=(v)
    $ta[-1] = 9
    @x = v
  end
end
$ta = [1, 2, 3]
t = TailBox.new
*t.x, c = $ta
p [t.x, c]
$ta = [1, 2, 3, 4]
t.x, *r, c = $ta
p [t.x, r, c]

# String, Float and object Arrays; a writer that empties the Array
class StrBox
  attr_reader :x
  def x=(v)
    $sa[1] = "z"
    @x = v
  end
end
$sa = ["a", "b"]
s = StrBox.new
s.x, c = $sa
p [s.x, c]
class FltBox
  attr_reader :x
  def x=(v)
    $fa[1] = 9.5
    @x = v
  end
end
$fa = [1.5, 2.5]
f = FltBox.new
f.x, c = $fa
p [f.x, c]
class Pt
  def initialize(n) = @n = n
  def n = @n
end
class PtBox
  attr_reader :x
  def x=(v)
    $pa[1] = Pt.new(9)
    @x = v
  end
end
$pa = [Pt.new(1), Pt.new(2)]
q = PtBox.new
q.x, c = $pa
p [q.x.n, c.n]
class ClearBox
  attr_reader :x
  def x=(v)
    $ca.clear
    @x = v
  end
end
$ca = [1, 2, 3]
e = ClearBox.new
e.x, c, *r = $ca
p [e.x, c, r]

# two writers; instance variables around a writer in a method
class TwoBox
  attr_reader :x, :y
  def x=(v)
    $wa[1] = 9
    @x = v
  end
  def y=(v)
    $wa[2] = 8
    @y = v
  end
end
class IvarRun
  def go(w)
    @a, w.x, @b = $wa
    [@a, w.x, @b]
  end
end
$wa = [1, 2, 3]
w = TwoBox.new
w.x, w.y, c = $wa
p [w.x, w.y, c]
$wa = [1, 2, 3]
p IvarRun.new.go(TwoBox.new)

# a boxed right side, a boxed writer receiver
class BoxedBox
  attr_reader :x
  def x=(v)
    $ba[1] = 9
    @x = v
  end
end
class PlainBox
  attr_accessor :x
end
$ba = [1, 2]
v = ARGV.size > 5 ? 1 : $ba
u = BoxedBox.new
u.x, c = v
p [u.x, c]
$ba = [1, 2]
u = ARGV.size > 5 ? PlainBox.new : BoxedBox.new
u.x, c = $ba
p [u.x, c]

# an object right side whose to_ary Array the writer changes
class Pair
  def to_ary = ($ba = [1, 2])
end
y = BoxedBox.new
y.x, c = Pair.new
p [y.x, c]

# a user []=, and a Hash key whose #hash changes the Array
class Store
  def initialize = @h = {}
  def [](k) = @h[k]
  def []=(k, v)
    $xa[1] = 9
    @h[k] = v
  end
end
class Key
  def hash
    $xa[1] = 9
    1
  end
  def eql?(o) = o.is_a?(Key)
end
$xa = [1, 2]
st = Store.new
st[0], c = $xa
p [st[0], c]
$xa = [1, 2]
h = {}
h[Key.new], c = $xa
p [h.values, c]

# an index store into the right side Array itself, one key and two
a = [1, 2]
a[1], c = a
p [a, c]
a = [5, 6, 7]
a[2], a[0], d = a
p [a, d]
a = [1, 2, 3, 4]
a[0, 2], b, c = a
p [a, b, c]
bv = ARGV.size > 5 ? {} : [1, 2]
bv[1], c = bv
p [bv, c]

# nested targets: an inner to_ary that changes the outer Array, from an
# object right side, a boxed one, an Array variable, among the rights,
# two levels deep, before a splat
class Inner
  def to_ary
    $na[1] = 9
    [1, 2]
  end
end
class Outer
  def to_ary = ($na = [Inner.new, 3])
end
(a, b), c = Outer.new
p [a, b, c]
ov = ARGV.size > 5 ? 1 : Outer.new
(a, b), c = ov
p [a, b, c]
$na = [Inner.new, 3]
(a, b), c = $na
p [a, b, c]
class TailInner
  def to_ary
    $ra[-1] = 9
    $ra << 10
    [1, 2]
  end
end
$ra = [0, TailInner.new, 3]
*r, (b, c), d = $ra
p [r, b, c, d]
$ra = [TailInner.new, 3, 4]
(a, b), *r = $ra
p [a, b, r]
class DeepInner
  def to_ary
    $mid[1] = 8
    $da[1] = 9
    [1, 2]
  end
end
$mid = [DeepInner.new, 3]
$da = [$mid, 4]
((a, b), c), d = $da
p [a, b, c, d]

# a writer inside a nested target, with a splat, from an Array variable,
# a literal and a boxed right side
class NestBox
  attr_reader :x
  def x=(v)
    $inner[1] = 9
    $oa[1] = 8
    @x = v
  end
end
$inner = [1, 2, 3]
$oa = [$inner, 3]
m = NestBox.new
(m.x, *r), c = $oa
p [m.x, r, c]
$inner = [1, 2]
a, (m.x, b) = 0, $inner
p [a, m.x, b]
$inner = [1, 2]
nv = ARGV.size > 5 ? 1 : [0, $inner]
a, (m.x, b) = nv
p [a, m.x, b]

# in a loop, the Array built each time
class LoopBox
  attr_reader :x
  def x=(v)
    $la[1] = 0
    @x = v
  end
end
lb = LoopBox.new
sum = 0
3.times do |i|
  $la = [i, i + 10]
  lb.x, c = $la
  sum += lb.x + c
end
p sum

# a writer last, or before a bare splat: nothing reads after it
class LastBox
  attr_reader :x
  def x=(v)
    $za[0] = 9
    @x = v
  end
end
$za = [1, 2]
z = LastBox.new
c, z.x = $za
p [c, z.x, $za]
$za = [1, 2]
z.x, = $za
p [z.x, $za]
$za = [1, 2, 3]
z.x, * = $za
p [z.x, $za]

# a right side the assignment builds, which no store can reach: a split, a
# divmod, a literal and a literal nested one
class FreshBox
  attr_reader :x, :y
  def x=(v)
    @x = v
  end
  def y=(v)
    @y = v
  end
end
f = FreshBox.new
f.x, c = "a,b".split(",")
p [f.x, c]
f.x, f.y = 17.divmod(5)
p [f.x, f.y]
(f.x, f.y), c = [3, 4], 5
p [f.x, f.y, c]
((f.x, d), f.y), c = [[6, 7], 8], 9
p [f.x, d, f.y, c]

# each_slice and each_cons with a block answer the receiver itself, which
# the writer changes
class SliceBox
  attr_reader :x
  def x=(v)
    $sa[1] = 9
    @x = v
  end
end
sb = SliceBox.new
$sa = [1, 2]
sb.x, c = $sa.each_slice(1) { }
p [sb.x, c]
$sa = [1, 2]
sb.x, c = $sa.each_cons(1) { }
p [sb.x, c]
# ...so a change of what each_slice answers changes the receiver
sl = [1, 2]
sl.each_slice(1) { }.map! { _1.to_s }
p sl
