# An alias made before a prepend holds the class's own method, which the
# prepended module's copy shadows for a call of the original name only: a
# Method of the alias calls and reports the class's body.
module M
  def foo = "M+" + super
end
class C
  def foo = "C"
  alias_method :bar, :foo
  prepend M
end
class D < C; end
c = C.new
p c.bar, c.foo
m = c.method(:bar)
p m.call, m.owner, m.source_location[1], m.original_name
p C.instance_method(:bar).bind(c).call
p D.new.method(:bar).call
