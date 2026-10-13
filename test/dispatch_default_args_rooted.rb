# spinel: gc-stress
# A call dispatched over a receiver of several classes, passing no
# arguments, fills the parameters it leaves empty: an empty rest is a new
# Array and an empty **kwrest a new Hash, each made in the C argument list.
# With both, the second's allocation freed the first under
# SPINEL_GC_STRESS=2.
class Img
  def a(**o) = o
  def b(*r, **o) = [r, o]
  def c(x = 1, *r, **o) = [x, r, o]
end
class Other
  def a = 0
  def b = 0
  def c = 0
end
y = [Img.new, Other.new][0]
p y.a, y.b, y.c
z = [Other.new, Img.new][1]
p z.b, z.c
