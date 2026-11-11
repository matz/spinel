# A class's own `raise` answers a value where it is called receiverless, so
# a branch that calls it is no diverging raise: its value is the method's
class U
  def raise(*a, **kw) = [:u, a, kw]
  def go(h) = raise(1, **h)
  def go3(h) = raise(1, 2, 3, **h)
  def pick(c) = (c.size > 5 ? 1 : raise(c.size))
end
class W
  def raise(*a) = [:w, a]
  def pick(c) = c.empty? ? raise(:empty) : c.first
  def fail_pick(c) = c.empty? ? 0 : fail(c.size)
  def fail(*a) = [:wf, a]
end
u = U.new
p u.go({})
p u.go({cause: 1})
p u.go3({})
p u.go3({zz: 1})
p u.pick([1])
p u.pick([1, 2, 3, 4, 5, 6])
w = W.new
p w.pick([])
p w.pick([4])
p w.fail_pick([1, 2])
def top(c) = c ? 1 : raise(ArgumentError, "top")
p top(true)
begin
  top(false)
rescue ArgumentError => e
  p e.message
end
