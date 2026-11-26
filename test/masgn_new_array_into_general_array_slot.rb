# A new Array a builtin answers, stored through a multiple assignment's
# target into a slot that a store through a parameter makes the general
# Array, is converted at the store, as a plain assignment converts it.

def y1(a) = yield(a)
def m1(x) = x << "q"

# a local, the store in a block
w, u = "ab".bytes, 2
y1(w) { |x| x << "q" }
p w, u

# a local, the store in a method
w2, u2 = "cd".bytes, 3
m1(w2)
p w2, u2

# a target after a splat
h3, *r3, w3 = 0, 1, "ef".bytes
m1(w3)
p h3, r3, w3

# an ivar of a class, the store in a block and in a method
class K
  attr_reader :w, :v
  def initialize = (@w, @n, @v = "gh".bytes, 1, "ij".bytes)
  def go = y1(@w) { |x| x << "q" }
  def go2 = m(@v)
  def y1(a) = yield(a)
  def m(a) = a << "r"
end
k = K.new
k.go
k.go2
p k.w, k.v

# the main object's ivar
@w4, @u4 = "kl".bytes, 4
y1(@w4) { |x| x << "q" }
p @w4, @u4

# two new Arrays, or a new Array and a String built after it: the
# conversion of the first allocates while the second waits for its store
w5, u5 = "mn".bytes, "op".bytes
y1(w5) { |x| x << "q" }
y1(u5) { |x| x << 1.5 }
p w5, u5
@w6, @u6 = "qr".bytes, "s" * 3
y1(@w6) { |x| x << "q" }
p @w6, @u6
@w7, @u7 = "tu".bytes, "vw".bytes
y1(@w7) { |x| x << "q" }
y1(@u7) { |x| x << 2.5 }
p @w7, @u7
class J
  attr_reader :w, :u
  def initialize = (@w, @u = "xy".bytes, "za".bytes)
  def go = (y1(@w) { |x| x << "q" }; y1(@u) { |x| x << 3.5 })
  def y1(a) = yield(a)
end
j = J.new
j.go
p j.w, j.u

# an attribute target whose field the new Array reaches boxed, before and
# after another target
class B8; attr_accessor :items; end
b8 = B8.new
b8.items, u8 = "ab".bytes, 5
y1(b8.items) { |x| x << "q" }
p b8.items, u8
b9 = B8.new
u9, b9.items = 6, Array.new(2, 1)
y1(b9.items) { |x| x << "r" }
p b9.items, u9

# the assignment as a value: its element is the Array its target holds,
# before and after a splat, in a method's value too
r10 = (w10, u10 = "cd".bytes, 0)
y1(w10) { |x| x << "q" }
r10[0] << 7
p w10, r10, r10[0].equal?(w10)
r11 = (h11, *s11, w11 = 0, 1, "ef".bytes)
y1(w11) { |x| x << "r" }
p r11, r11[2].equal?(w11)
def mk12(o)
  r = (o.items, u = "gh".bytes, 1)
  [r, r[0].equal?(o.items)]
end
b12 = B8.new
p mk12(b12)
y1(b12.items) { |x| x << "s" }
p b12.items

# an attribute target in a parent's method, whose writer a subclass
# overrides with one that takes the Integer Array: the store dispatches
# on the class, and the subclass's writer takes the element, beside a
# store of another kind through the parent's field
class B13
  attr_accessor :items
  def fill = (self.items, u = "ij".bytes, 0)
end
class S13 < B13
  def items=(v)
    @w = v
  end
  attr_reader :w
end
s13 = S13.new
p s13.fill, s13.w
b13 = B13.new
b13.fill
y1(b13.items) { |x| x << "q" } if ARGV.size > 5
p b13.items

# ...and in statement position, where the element is a new Array nothing
# else holds: the subclass's writer takes it as the Integer Array and the
# parent's field as the general Array, which a push of another kind reaches
class B14
  attr_accessor :items
  def fill
    self.items, u = "kl".bytes, 0
    nil
  end
end
class S14 < B14
  def items=(v)
    @w = v
  end
  attr_reader :w
end
s14 = S14.new
s14.fill
p s14.w
b14 = B14.new
b14.fill
y1(b14.items) { |x| x << "q" }
p b14.items
o14 = ARGV.size > 5 ? B14.new : S14.new
o14.items, u14 = "mn".bytes, 0
p o14.w
c14 = B14.new
c14.items, u14 = "op".bytes, 0
y1(c14.items) { |x| x << "r" }
p c14.items

# ...and read for its value, a method's value too: the element the value
# answers is the Array the parent's field holds
b15 = B13.new
r15 = b15.fill
y1(b15.items) { |x| x << "q" }
p b15.items, r15, r15[0].equal?(b15.items)
c15 = B14.new
r15b = (c15.items, u15 = "qr".bytes, 0)
r15b[0] << "s"
p c15.items, r15b[0].equal?(c15.items)
