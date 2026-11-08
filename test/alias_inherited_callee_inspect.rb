# An alias of an inherited def is a method of the alias's class made of the
# def: __method__ answers the def's name, __callee__ the alias's, and an
# UnboundMethod of it renders with the def's class as its owner, a Method of it
# the receiver's class ahead of that.
class A
  def d(x = 0) = [__method__, __callee__]
  def f = [__method__, __callee__]
  alias_method :g, :f
end
class B < A
  alias_method :e, :d
  alias_method :h, :g
end
p B.new.d, B.new.e, B.new.h, B.new.g, B.new.f
puts B.instance_method(:e).inspect.sub(/ \S+:\d+>\z/, ">")
p B.instance_method(:e).owner, B.instance_method(:e).name, B.instance_method(:e).original_name
puts B.new.method(:e).inspect.sub(/ \S+:\d+>\z/, ">")
