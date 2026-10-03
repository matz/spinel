# A boxed nil unboxed into an Integer slot lands as the slot's nil
# sentinel, never as 0. Each value below reaches an Integer-typed slot
# through a box: the value `p` hands back, a proc's return, a catch value,
# a safe-navigation call's result. Read as 0, it printed 0, answered false
# to nil?, and added 1 where CRuby raises. nil.to_i stays 0: that is a
# method call, not an unbox.

def show(c)
  v = 1 if c
  p v
end
r = show(false)
p r
p r.nil?
p(r + 1) rescue p $!.class
p r.to_i
s = show(true)
p s

pr = proc { |a, b| p b }
q = pr.call(1)
p q
q = pr.call(1, 2)
p q

t = catch(:x) do
  loop do
    throw :x, nil
  end
end
p t
t = catch(:x) do
  loop do
    throw :x, 42
  end
end
p t

class Unit
  def n = 5
end
def nav(o)
  p o&.n
end
p nav(nil)
p nav(Unit.new)

def each2
  yield 1
  yield nil
end
each2 { |a| w = p(a); p w.nil? }

xs = [1, nil, "a"]
p xs[1].to_i
p nil.to_i
