# method in a class whose method_missing answers a name a later include
# or prepend defines. Where no respond_to_missing? answers it, CRuby
# raises NameError, naming the receiver's class, and after the include
# makes a Method of the module's method.
module V; def a = 1; end
class K
  def method_missing(n, *r) = n == :a ? :mm : super
end
class J < K; end
k = K.new
begin; k.method(:a); rescue NameError => e; p e.class, e.message; end
begin; J.new.method(:a); rescue NameError => e; p e.message; end
p k.a
class K; include V; end
m = k.method(:a)
p m.call, m.owner, k.a, J.new.method(:a).call

# a subclass defines the name
module X; def c = 1; end
class M
  def method_missing(n, *r) = n == :c ? :mm : super
end
class MS < M; def c = :ms; end
o = M.new
begin; o.method(:c); rescue NameError => e; p e.message; end
class M; include X; end
p o.method(:c).call, MS.new.method(:c).call, MS.new.method(:c).owner

# with a respond_to_missing? that answers it, beside a subclass that
# defines the name: a Method made before a later include or prepend, not
# called
module W; def b = 1; end
class N
  def method_missing(n, *r) = n == :b ? :mm : super
  def respond_to_missing?(n, p = false) = n == :b || super
end
class NS < N; def b = :ns; end
nn = N.new
p nn.method(:b).class, nn.method(:b).owner
class N; include W; end
p nn.b, NS.new.b

class P
  def method_missing(n, *r) = n == :b ? :mm : super
  def respond_to_missing?(n, p = false) = n == :b || super
end
class PS < P; def b = :ps; end
pp = P.new
p pp.method(:b).class
class P; prepend W; end
mp = pp.method(:b)
p mp.call, mp.owner, pp.b
