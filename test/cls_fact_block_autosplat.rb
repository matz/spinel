# A block with |x, y| spreads one yielded Array over its parameters:
# each parameter can hold the classes of that Array's elements.
class A; def foo = "A"; end
class C; def foo = "C"; end
def y2 = yield([A.new, C.new])
y2 { |a, b| puts b.foo }
h = {k: [C.new, 1]}
h.each_value { |x, y| puts x.foo }
[[A.new, [C.new]]].each { |x, (y)| puts y.foo }
[[A.new, C.new]].each_with_index { |(x, y), i| puts y.foo }
[[A.new, C.new]].map { |x, y| y }.each { |z| puts z.foo }
[[1, C.new]].each { |x, *r| puts r[0].foo }
class F; def n = 7; end
a = [[F.new, 1], [F.new, 2]]
a.each { |x, y| p x.n }
p a.map { |x, y| [x.n, y] }
p a.map { |x, | x.n }
def y3 = yield([1, A.new, C.new])
y3 { |a, *r| puts r.map(&:foo).join(",") }
y3 { |a, b = nil, c| puts c.foo }
[[A.new, C.new]].each { puts _2.foo }
y2 { puts _2.foo }
