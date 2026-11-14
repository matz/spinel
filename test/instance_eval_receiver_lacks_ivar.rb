# An instance_eval/exec block in a method reads an ivar the receiver's
# class never holds: nil, as on that object. The read took the field of the
# receiver's struct, which has none, and the C did not build; on a receiver
# of two classes the arm of the class without it did the same.
class O
  def initialize = (@x = 99)
  def foo = 1
end

class P
  def foo = 2
end

class T
  def initialize = (@x = 1; @s = "t")
  def go = P.new.instance_eval { [@x, @s, foo] }
  def go_exec = P.new.instance_exec(@x) { |a| [a, @x] }
  def either(i) = [O.new, P.new][i].instance_exec(@x) { |a| [a, @x, foo] }
  def value(i) = [O.new, P.new][i].instance_exec { foo; @x }
end

t = T.new
p t.go, t.go_exec
p t.either(0), t.either(1)
p t.value(0), t.value(1)
