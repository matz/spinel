# nil handed to an Integer or Float parameter through a boxed receiver's
# dispatch -- an object read out of an Array, whose class picks the method --
# by keyword or by position, written as nil or as a call answering nil. The
# arm read the zero under the boxed nil's tag, and the methods' parameters
# were not marked as ones nil can reach, so the value came out 0 (or NaN).
class C; def k(a:, b: 0) = [a, b]; end
class D; def k(a:, b: 0) = [a, b]; end
p [C.new, D.new].map { |o| o.k(b: puts("x"), a: 3) }

class E; def m(a, b = 0) = [a, b]; end
class F; def m(a, b = 0) = [a, b]; end
o = [E.new, F.new][ARGV.size]
p o.m(3, nil), o.m(3, 4), o.m(5)

class G; def f(a:, b: 0.5) = [a, b, b.nil?]; end
class H; def f(a:, b: 0.5) = [a, b, b.nil?]; end
g = [G.new, H.new][ARGV.size]
p g.f(b: nil, a: 1), g.f(b: 2.5, a: 1), g.f(a: 1)
