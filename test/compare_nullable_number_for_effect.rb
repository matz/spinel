# An Integer or Float that may be nil, compared where the comparison's answer
# does not depend on its value, is evaluated for effect only: its nil is no
# TypeError.
class K
  def initialize; @a = 1; end
  def ne(x) = (self != x)
  def eq(x) = (self == x)
end

def maybe_int(f) = f ? 3 : nil
def maybe_float(f) = f ? 2.5 : nil

o = K.new
p o.ne(1), o.ne(nil)
p o.eq(1), o.eq(nil)

t = Time.at(0)
x = maybe_int(false)
p t == x, t != x
y = maybe_float(false)
p t == y, t != y
p t == maybe_int(true)

h = {a: 1}
p h == x, h != x

s = "abc"
i = s.index("z")
p o != i, o == i
p :sym == i, :sym != i
