# `k.instance_method(:m).bind_call(obj)` on a Class value only the run time
# knows binds the method the capture finds where it runs: an attribute reader
# the class had when the Method was made, a method an inherited class
# redefines later, and an alias made after the capture each answer the
# captured one on the earlier calls and the later one on the later calls.
class A
  attr_reader :m
  def initialize = @m = :attr_a
end
class B
  def m = :def_b
end
ks = [A, B]
p ks.map { |k| k.instance_method(:m).bind_call(k.new) }
class A
  def m = :def_a
end
p ks.map { |k| k.instance_method(:m).bind_call(k.new) }

class P
  def m = :p1
end
class Q < P; end
class R
  def m = :r
end
kz = [Q, R]
p kz.map { |k| k.instance_method(:m).bind_call(k.new) }
class P
  def m = :p2
end
p kz.map { |k| k.instance_method(:m).bind_call(k.new) }

class X
  def n = :n
  def m = :x1
end
class Y
  def m = :y
end
kw = [X, Y]
p kw.map { |k| k.instance_method(:m).bind_call(k.new) }
class X
  alias m n
end
p kw.map { |k| k.instance_method(:m).bind_call(k.new) }
