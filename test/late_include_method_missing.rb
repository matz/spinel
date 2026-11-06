# A call made before a later include of a module, in a class whose chain
# has no method of the name but a method_missing: the hidden method the
# include's copy stands in front of finds nothing, so the call reaches
# method_missing, as in CRuby, and a Method made there calls it too.
module V; def a = 1; end
class K
  def method_missing(n, *r) = n == :a ? :mm : super
end
k = K.new
p k.a
class K; include V; end
p k.a

# the include under a condition, and a Method made before it
module W; def b = 1; end
class L
  def respond_to_missing?(n, p = false) = n == :b || super
  def method_missing(n, *r) = n == :b ? :mm : super
end
l = L.new
p l.b, l.method(:b).call
if ARGV.empty?
  class L; include W; end
end
p l.b, l.method(:b).call
module W; def b = 2; end
p l.b, l.method(:b).call

# a Method made before the include keeps calling method_missing after it,
# and after a later version of the module's method
module X; def c(x) = [:x1, x]; end
class M
  def method_missing(n, *r) = n == :c ? [:mm, *r] : super
  def respond_to_missing?(n, p = false) = n == :c || super
end
o = M.new
m0 = o.method(:c)
p m0.call(3), m0.arity
class M; include X; end
m1 = o.method(:c)
p m0.call(4), m1.call(5)
module X; def c(x) = [:x2, x]; end
m2 = o.method(:c)
p [m0.call(6), m1.call(7), m2.call(8)]
p [m0.owner, m1.owner, m2.owner]
p [m0.arity, m1.arity, m2.arity]
