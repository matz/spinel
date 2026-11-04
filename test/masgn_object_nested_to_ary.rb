# An object a multiple assignment with a nested target takes apart
# converts through its to_ary, and each nested target converts the
# element it takes in turn: the object is not the one element. An
# object without a to_ary, or one defined after the assignment, is.
class Pair
  def initialize(a, b); @a = a; @b = b; end
  def to_ary = [@a, @b]
end
class Plain
  def initialize(v) = @v = v
end
class BadAry
  def to_ary = 5
end
class Counted
  $n = 0
  def initialize(a) = @a = a
  def to_ary = ($n += 1; [@a, 0])
end
class Late
  def initialize(a) = @a = a
end
(a, b), c = Pair.new(Pair.new(1, 2), 3)
p [a, b, c]
((a, b), c), d = Pair.new(Pair.new(Pair.new(1, 2), 3), 4)
p [a, b, c, d]
(a, b), c = Pair.new([7, 8], 9)
p [a, b, c]
def mk = Pair.new(Pair.new(9, 10), 11)
(a, b), c = mk
p [a, b, c]
(a, b), c = Plain.new(1)
p [a.class, b, c]
begin
  x, y = BadAry.new
  p [x, y]
rescue TypeError => e
  p e.class
end
(a, b), c = Counted.new(Counted.new(1))
p [a, b, c, $n]
(a, b), c = Late.new(Late.new(2))
p [a.class, b, c]
class Late
  def to_ary = [@a, :late]
end
x, y = Late.new(1)
p [x, y]
r = ((a, b), c = Pair.new(Pair.new(1, 2), 3))
p r.class
