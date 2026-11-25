# A boxed receiver's merge with a splat among its operands: every member
# of the list is an operand, converted or refused as a typed Hash's are

class H
  def to_hash = { t: 9 }
end
w = [{ a: 1 }, 0][0]
hs = [{ b: 2 }, { c: 3 }]
p w.merge(*hs)
e = []
p w.merge(*e), w.merge({ z: 0 }, *hs), w.merge(*hs, *[{ d: 4 }])
p w.merge(*[{ a: 5 }]) { |_, x, y| x + y }
p w.merge(*[H.new])
p w

# the result read on, and two calls in one statement
y = w.merge(*hs)
p y[:c], y.size
p [w.merge(*hs), w.merge(*e)]

# a rest parameter, through levels
def m3(x, *r) = x.merge(*r)
def m2(x, *r) = m3(x, *r)
def m1(x, *r) = m2(x, *r)
p m1(w, { d: 4 }), m1(w)
def anon(x, *) = x.merge(*)
p anon(w, { f: 6 })

# errors
n = [nil, 0][0]
begin; n.merge(*hs); rescue NoMethodError => x; p x.message; end
s = ["str", 0][0]
begin; s.merge(*hs); rescue NoMethodError => x; p x.message; end
begin; w.merge(*[nil]); rescue TypeError => x; p x.message; end
begin; w.merge(*[1]); rescue TypeError => x; p x.message; end
