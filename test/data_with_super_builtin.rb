# `super` from a Data's own `with` that no ancestor defines reaches the
# builtin Data#with, as in CRuby. It raised "super: no superclass method
# 'with'" at run time.
D = Data.define(:a, :b)
class D
  def with(**kw) = super
end
d = D.new(a: 1, b: 2)
p d.with(a: 5)
p d.with.equal?(d)
p d.with(**{}).equal?(d)

E = Data.define(:a, :b)
class E
  def with(**kw)
    super(**kw)
  end
end
e = E.new(a: 1, b: "x")
p e.with(b: "y")
p e.with(a: 3, b: "z")
begin
  e.with(c: 1)
rescue ArgumentError => ex
  p ex.message
end

F = Data.define(:a, :b)
class F
  def with(a: 0, **rest)
    super(a: a * 10, **rest)
  end
end
p F.new(a: 1, b: 2).with(b: 9)
p F.new(a: 1, b: 2).with(a: 4)

G = Data.define(:n, :tag)
class G
  def with(n:, **rest) = super
end
p G.new(n: 1, tag: :t).with(n: 2)
p G.new(n: 1, tag: :t).with(n: nil, tag: :u)

H = Data.define(:v)
class H
  def with(**kw)
    r = super
    [r, r.equal?(self)]
  end
end
p H.new(v: 1).with(v: 2)
p H.new(v: 1).with

M = Data.define(:x)
class M
  def with(**kw) = super(x: 7)
end
p M.new(x: 1).with

W = Data.define(:n)
class W
  def with(**kw) = super
end
p W.new(n: 1).with(n: "now a String")
p W.new(n: 2).with(n: 2.5)

P = Data.define(:x, :y)
class P
  def with(**kw) = super()
end
q = P.new(x: 1, y: 2)
p q.with(x: 9).equal?(q)
R = Data.define(:x, :y)
class R
  def with(**kw) = super
end
r = R.new(x: nil, y: 2)
p r.with(y: 3), r.with(x: 4).with(x: nil)

# a keyword that defaults to nil reaches an Integer or Float member as nil,
# through `super(k: v)` and through a bare super
NI = Data.define(:n, :m)
class NI
  def with(n: nil) = super(n: n)
end
p NI.new(n: 1, m: 2).with(n: 5), NI.new(n: 1, m: 2).with
NB = Data.define(:n, :m)
class NB
  def with(n: nil) = super
end
p NB.new(n: 1, m: 2).with(n: 5), NB.new(n: 1, m: 2).with(n: nil)
FX = Data.define(:f)
class FX
  def with(f: nil) = super(f: f)
end
p FX.new(f: 1.5).with(f: 2.5), FX.new(f: 1.5).with
FB = Data.define(:f, :g)
class FB
  def with(f: nil, **rest) = super
end
p FB.new(f: 1.5, g: 2).with(g: 3), FB.new(f: 1.5, g: 2).with(f: 0.5)
FC = Data.define(:f)
class FC
  def with(f: 3.0) = super(f: f > 2 ? nil : f)
end
p FC.new(f: 1.0).with, FC.new(f: 1.0).with(f: 1.5)
