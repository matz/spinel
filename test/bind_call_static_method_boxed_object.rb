# An UnboundMethod made of a class's method before it is redefined keeps the
# first definition when it is bound to a boxed object; a Method made after the
# redefinition binds the second.
class A
  def m = :a1
end
um = A.instance_method(:m)
class A
  def m = :a2
end
box = [A.new, 1].first
p um.bind_call(box)
p A.instance_method(:m).bind_call(box)
um2 = A.instance_method(:m)
p um2.bind_call([A.new, :x].first)
