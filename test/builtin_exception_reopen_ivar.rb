# An instance variable in a method a program adds to a builtin exception
# class lives in the map keyed by the object, as a String's or an Array's
# does: it is the exception's own, survives a raise and a rescue, is copied
# by dup, and an attr_reader reads it. Taken as a struct field, it named a
# member the builtin exception has not, and the C did not compile (#11890).
class RuntimeError
  def initialize(m = nil)
    super(m)
    @own = 1
  end
  attr_reader :own
end
class StandardError
  def tag!(v)
    @tag = v
    self
  end
  def tag = @tag
end

begin
  raise RuntimeError.new("boom")
rescue => e
  p e.message, e.own
end
p RuntimeError.new("x").own

a = ArgumentError.new("a").tag!(:first)
b = ArgumentError.new("b")
p a.tag, b.tag
b.tag!(:second)
p a.tag, b.tag
p a.instance_variable_get(:@tag), a.instance_variables
d = a.dup
d.tag!(:copy)
p a.tag, d.tag

begin
  raise KeyError.new("k").tag!(:in_flight)
rescue KeyError => k
  p k.tag, k.message
end
