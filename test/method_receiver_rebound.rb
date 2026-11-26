# Method#receiver answers the value the Method was looked up on, after the
# local or ivar it was read from is rebound
def build = Class.new {}

# a class made at run time, and a module
k = build
first_k = k
mk = k.method(:new)
k = build
p mk.receiver.equal?(first_k), mk.receiver.equal?(k)
mod = Comparable
mm = mod.method(:instance_methods)
mod = Kernel
p mm.receiver

# an object
class Foo; def x = 1; end
o = Foo.new
first_o = o
mo = o.method(:x)
o = Foo.new
p mo.receiver.equal?(first_o), mo.receiver.equal?(o)

# a String
s = +"a"
first_s = s
ms = s.method(:upcase)
s = +"b"
p ms.receiver, ms.receiver.equal?(first_s), ms.call

# an Array, a Hash, an Integer
a = [1]
ma = a.method(:size)
a = [1, 2]
p ma.receiver, ma.call
h = {a: 1}
mh = h.method(:keys)
h = {}
p mh.receiver
i = 5
mi = i.method(:succ)
i = 9
p mi.receiver, mi.call

# ivars, and two swapped in one statement
class Holder
  def initialize; @k = Class.new; @o = Foo.new; end
  def go
    k0 = @k
    o0 = @o
    mk = @k.method(:new)
    mo = @o.method(:x)
    @k = Class.new
    @o = Foo.new
    p mk.receiver.equal?(k0), mo.receiver.equal?(o0), mo.receiver.equal?(@o)
  end
end
Holder.new.go
c = build
d = build
mc = c.method(:new)
md = d.method(:new)
c, d = d, c
p [mc.receiver.equal?(d), md.receiver.equal?(c)]

# read inside a block, after a write in one
j = build
first_j = j
mj = j.method(:new)
[1].each { j = build }
rd = -> { mj.receiver }
p rd.call.equal?(first_j), rd.call.equal?(j)
