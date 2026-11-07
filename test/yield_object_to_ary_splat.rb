# An object whose class defines to_ary, yielded alone to a block (or
# passed alone to a proc, or taken by `for a, b in`) of several
# parameters, is taken apart through its to_ary, as CRuby does.
class W; def to_ary = [1, 2]; end
class N; def to_ary = nil; end
class S < W; end
class Q; def to_ary = [:q, "s", 3.5]; end
def m(v) = yield(v)
m(W.new) { |x, y| p [x, y] }
m(W.new) { |x| p x.class }
m(W.new) { |x, *r| p [x, r] }
m(W.new) { |x, y = 5, z| p [x, y, z] }
m(N.new) { |x, y| p [x.class, y] }
m(S.new) { |x, y| p [x, y] }
m(Q.new) { |a, b, c| p [a, b, c] }
def twice
  yield W.new
  yield [7, 8]
end
twice { |a, b| p [a, b] }
def rec(n, &b)
  return yield(W.new) if n == 0
  rec(n - 1, &b)
end
rec(2) { |a, b| p [a, b] }
l = lambda { |a, b| [a, b] }
p((m(W.new, &l) rescue $!.class))
def each_w
  [W.new, W.new].each { |w| yield w }
end
each_w { |a, b| p a + b }
p m(W.new) { |a, b| a * 10 + b }
pr = proc { |x, y| [x, y] }
p pr.call(W.new)
p pr.call(N.new).map(&:class)
p proc { |x, *r| [x, r] }.call(W.new)
p proc { |x| x.class }.call(W.new)
for u, v in [W.new]; p [u, v]; end
for u, v in [W.new, [3, 4], N.new]; p [u.class, v]; end
