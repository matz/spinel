# A multiple assignment writes through a target that is no variable: `o.v, b
# = x, 1` calls o.v = x and `a[0], b = x, 1` calls a[0] = x. What is read
# back can be x, and a call on it dispatches to x's class's method. The
# value may be an Array taken apart, the target may be nested or the splat's
# rest, and the setter or the []= may be the program's own.
class A; def foo = "A"; end
class B; def foo = "B"; end
class C; def foo = "C"; end

class Box
  attr_accessor :v

  def initialize(v)
    @v = v
  end
end

class Cell
  def initialize
    @v = A.new
  end

  def v = @v

  def v=(x)
    @v = x
  end

  def []=(k, x)
    @v = x
  end
end

# an attribute, an element of an Array and of a Hash
o = Box.new(A.new)
o.v, n = B.new, 1
puts o.v.foo
puts n

a = [A.new]
a[0], n = C.new, 2
puts a[0].foo

h = { k: A.new }
h[:k], n = B.new, 3
puts h[:k].foo

# the value an Array, the target last, and a slice
o2 = Box.new(A.new)
o2.v, n = [C.new, 4]
puts o2.v.foo

a2 = [A.new]
n, a2[1] = 5, B.new
a2.each { |x| puts x.foo }

a3 = [A.new]
a3[1, 0], n = [C.new], 6
a3.each { |x| puts x.foo }

# a nested target, and the splat's rest
a4 = [A.new]
a4[1], (n, a4[2]) = B.new, [7, C.new]
a4.each { |x| puts x.foo }

o3 = Box.new([A.new])
*o3.v, n = B.new, C.new, 8
o3.v.each { |x| puts x.foo }

a5 = [[A.new]]
*a5[0], n = C.new, B.new, 9
a5[0].each { |x| puts x.foo }

# the program's own setter and []=
c = Cell.new
c.v, n = B.new, 10
puts c.v.foo

d = Cell.new
d[0], n = C.new, 11
puts d.v.foo
puts n
