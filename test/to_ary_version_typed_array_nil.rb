# A to_ary whose version at the assignment answers an Array of one element
# type that may be nil (a global or an attribute holding [1, 2] or nil)
# keeps the value when it answers nil, though a later version answers an
# Array of mixed elements: the version the assignment runs decides how the
# answer is taken apart, for String and for a program's class, at the top
# level, in a method, two in one statement, and over a value that may be nil.
$h = nil
class String
  def to_ary = $h
end
def pick(n) = n ? nil : +"s"
def g(s) = (a, b = s; [a, b])
a, b = +"s"
p [a, b]
p g(+"s")
a, b = pick(true)
p [a, b]
$h = [1, 2]
a, b = +"t"
p [a, b]
p g(+"t")
x = (a, b = +"u"); y = (c, d = +"v")
p [a, b, c, d, x, y]
$h = nil
class String
  def to_ary = empty? ? nil : [self, 1]
end
a, b = +""
p [a, b]
p g(+""), g(+"z")

class Box
  attr_accessor :a
end
B = Box.new
class P
  def to_ary = B.a
end
def mk(n) = n ? nil : P.new
a, b = P.new
p [a.class, b]
a, b = mk(true)
p [a, b]
B.a = [3, 4]
a, b = mk(false)
p [a, b]
B.a = nil
class P
  def to_ary = [:x, 1].then { B.a ? _1 : nil }
end
a, b = mk(false)
p [a.class, b]
