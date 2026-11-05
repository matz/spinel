# The #super_method of a Method of an alias made before a prepend is the
# parent's method of the def's own name, whose body the alias holds; the
# parent's method is emitted though no call of the program names it.
module M
  def foo = "M+" + super
end
class P
  def foo = "P"
end
class C < P
  def foo = "C+" + super
  alias_method :bar, :foo
  prepend M
end
class Q < P
  def foo = "Q"
  alias_method :baz, :foo
  prepend M
end
c = C.new
p c.foo, c.bar, c.method(:bar).call
s = c.method(:bar).super_method
p s.owner, s.call, s.name
u = Q.instance_method(:baz).super_method
p u.owner, u.name, u.bind(Q.new).call
