# An alias of an inherited def under the def's own name is a method of the
# aliasing class made of it: the def's class stays its owner in a rendering.
class A
  def m(x = 0) = :m
end
class B < A
  alias_method :m, :m
end
p B.new.m
puts B.instance_method(:m).inspect.sub(/ \S+:\d+>\z/, ">")
puts B.new.method(:m).inspect.sub(/ \S+:\d+>\z/, ">")

# ...and one whose body calls super: the copy's super starts where the
# def's does, above the def's own class, never at the copy itself
class Base
  def s = "base"
end
class SA < Base
  def s = "SA+" + super
end
class SB < SA
  alias_method :s, :s
end
class SC < SA
  alias s s
  def via = s
end
puts SB.new.s, SC.new.via
