# `K.instance_method(:x).bind_call(obj)` where x is a member of a Struct or
# Data class reads (or, for a Struct, writes) the member, as it does for an
# attr reader: on the record constant, a class with a block, a subclass of
# `Struct.new`, a Data class, a local's record class and a Class value only
# the run time knows. A def of the member's name answers ahead of the member.
# A member is a reader with no attr declaration in a body, so the bind_call
# found no method of the class and raised NameError.
S = Struct.new(:d, :f)
s = S.new(1, 2)
p S.instance_method(:d).bind_call(s)
p S.instance_method(:f=).bind_call(s, 9)
p s.f
B = Struct.new(:g) do
  def twice = g * 2
end
p B.instance_method(:g).bind_call(B.new(4)), B.new(5).twice
class T < Struct.new(:e)
end
p T.instance_method(:e).bind_call(T.new(6))
class U < Struct.new(:h)
  def h = "own #{super}"
end
p U.instance_method(:h).bind_call(U.new(7))
D = Data.define(:x)
p D.instance_method(:x).bind_call(D.new(x: 8))
k = Struct.new(:y)
p k.instance_method(:y).bind_call(k.new(10))
def go(cls, o) = cls.instance_method(:d).bind_call(o)
p go(S, S.new(11, 0))
p((S.instance_method(:d).bind_call(B.new(1)) rescue $!.class))
