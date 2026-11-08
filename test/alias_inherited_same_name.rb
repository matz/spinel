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
