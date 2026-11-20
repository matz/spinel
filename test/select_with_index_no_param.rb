# A select.with_index or reject.with_index whose block names no element
# keeps the receiver's own element: the result read it from a block
# parameter that was not there, and the C did not build.
p [1, :a].select.with_index { true }
p [nil, 1].select.with_index { true }
p [:x, :y].reject.with_index { false }
p %w[a b].reject.with_index(1) { false }
p [1.5, nil].reject.with_index { false }
p (1..4).select.with_index { true }
p [[1, 2], [3]].select.with_index { true }

s = %w[a b c]; n = 0
t = s.select.with_index { (n += 1).odd? }
t << "z"
p t, s

Pt = Struct.new(:x)
p [Pt.new(1), Pt.new(2)].select.with_index { true }

# at a method's tail, the receiver boxed
def kk(a) = a.select.with_index { true }
p kk([1, :a]), kk(%w[q]), kk([2.5]), kk([[1]])
def rr(n) = (1..n).reject.with_index { false }
p rr(3)

# reject keeps the element it yielded, as it was before the block ran;
# select reads it back afterwards
s = %w[a b c]; p s.reject.with_index { s.shift; false }
q = [:a, :b, :c]; p q.reject.with_index { q.shift; false }
t = %w[a b]; p t.reject.with_index { t[0] = "Q"; false }
m = [1, :a, "z"]; p m.reject.with_index { m.shift; false }
x = %w[a b]; y = x.reject.with_index { false }; p y[0].equal?(x[0])
z = [true, false, nil]; p z.reject.with_index { z.pop; false }
w = %w[a b c]; p w.select.with_index { w.shift; true }
