# Method#super_method into a method the program defines on Object: the
# receiver binds as self and the arguments arrive as written; the name of a
# method behind a prepend is the one the program wrote.
class Object
  def greet(x) = "obj #{x}"
  def twice(x) = x * 2
  def plain = "obj"
end
class A
  def greet(x) = super(x) + "!"
  def twice(x) = x * 3
  def plain = super
end
p A.new.greet(1)
p A.new.method(:greet).super_method.owner
p A.new.method(:greet).super_method.call(2)
p A.new.method(:twice).super_method.call(21)
p A.new.method(:plain).super_method.call

module P
  def foo = :p
end
class B
  prepend P
  def foo = :b
end
sm = B.new.method(:foo).super_method
p sm.name
p sm.call
puts sm.inspect.sub(/ \S+:\d+>/, ">")
p sm.arity
p sm.owner

# an unbound method of Object binds to an instance of any class
p A.instance_method(:twice).super_method.bind(A.new).call(21)
p Object.instance_method(:twice).bind(A.new).call(21)
