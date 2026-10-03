# An Integer value that arrives boxed and can be nil -- the value `p`
# hands back, a Proc's call, a catch, a safe-navigation call -- is a
# nullable one: unboxed, its nil is the sentinel, and every operation on
# it answers as on nil. Unmarked, `-x` printed the sentinel's digits and
# `x > 1` answered false where CRuby raises.

def ops(x)
  p(x + 1) rescue p $!.class
  p(1 + x) rescue p $!.class
  p(-x) rescue p $!.class
  p(x.abs) rescue p $!.class
  p(x.to_i) rescue p $!.class
  p(x > 1) rescue p $!.class
  p(1 < x) rescue p $!.class
  p x.nil?
  p x
end

def show(c)
  v = 1 if c
  p v
end
x = show(false)
ops(x)
x = show(true)
ops(x)

pr = proc { |a, b| p b }
y = pr.call(1)
pr.call(1, 2)
p(-y) rescue p $!.class
p(y > 1) rescue p $!.class

z = catch(:x) do
  loop { throw :x, nil }
end
p(-z) rescue p $!.class
p(z > 1) rescue p $!.class
w = catch(:x) do
  loop { throw :x, 2 }
end
p(-w)

class Unit
  def n = 5
end
def nav(o)
  p o&.n
end
s = nav(nil)
nav(Unit.new)
p(-s) rescue p $!.class
p(s.abs) rescue p $!.class
p(s > 1) rescue p $!.class
t = nav(Unit.new)
p(-t)

def each2
  yield 1
  yield nil
end
each2 do |a|
  u = p(a)
  p(-u) rescue p $!.class
  p(u > 0) rescue p $!.class
end
