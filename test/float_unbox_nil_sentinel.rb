# A boxed nil unboxed into a Float slot lands as the slot's nil sentinel
# (a quiet NaN with payload 1), never as 0.0: the value `p` hands back, a
# Proc's call, a catch value, a safe-navigation call. Read as 0.0 it
# printed 0.0 and answered false to nil?. Every operation on it answers as
# on nil: arithmetic raises before the NaN payload can carry the nil into
# the result, and unary minus and abs raise before the sign bit can turn
# it into a plain NaN. A real NaN -- Float::NAN, 0.0/0.0, -Float::NAN --
# stays a Float through the same slots. nil.to_f stays 0.0.

def ops(x)
  p(x + 1.0) rescue p $!.class
  p(1.0 + x) rescue p $!.class
  p(x * 2.0) rescue p $!.class
  p(-x) rescue p $!.class
  p(x.abs) rescue p $!.class
  p(x.to_f) rescue p $!.class
  p(x > 1.0) rescue p $!.class
  p(1.0 < x) rescue p $!.class
  p x.nil?
  p x
end

def show(v)
  p v
end
ops(show(nil))
ops(show(1.5))
p show(nil).to_f

pr = proc { |a, b| p b }
y = pr.call(1.5)
p y
p y.nil?
p(y + 1.0) rescue p $!.class
p(-y) rescue p $!.class
p pr.call(1.5, 2.5)

z = catch(:x) do
  loop { throw :x, nil }
end
p z
p(z + 1.0) rescue p $!.class
z2 = catch(:x) do
  loop { throw :x, 4.5 }
end
p z2

class Unit
  def n = 5.5
end
def nav(o)
  p o&.n
end
s = nav(nil)
p s
p(s * 2.0) rescue p $!.class
p(s.abs) rescue p $!.class
p nav(Unit.new)

def each2
  yield 1.5
  yield nil
end
each2 do |a|
  u = p(a)
  p u.nil?
  p(u - 1.0) rescue p $!.class
end

# real NaNs are no nil
[Float::NAN, 0.0 / 0.0, -Float::NAN].each do |nan|
  n = show(nan)
  p n.nan?
  p n.nil?
  p n + 1.0
  p(-n)
  p n.abs
  c = catch(:x) { loop { throw :x, nan } }
  p c.nan?
end

xs = [1.5, nil, "a"]
p xs[1].to_f
p nil.to_f
