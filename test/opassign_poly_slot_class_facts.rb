# `lv OP= rhs` on a boxed local goes out to the runtime's operator dispatch,
# as `lv OP rhs` does: the operators of the classes the local can hold take
# a boxed argument, and no other class's. A running sum of a boxed reader
# (Floats and Integers) holds no Vec, so Vec#+ keeps its Vec parameter and
# Vec's Float members stay Floats (infer-test checks both). (A local that
# may hold a Vec is opassign_poly_slot_holds_class.rb.)
Vec = Struct.new(:x, :y) do
  def +(other) = Vec.new(x + other.x, y + other.y)
end
ORIGIN = Vec.new(0.5, 1.5)

class Reading
  attr_reader :x
  def initialize(x) = @x = x
end

readings = [Reading.new(1.5), Reading.new(2)]
sum = 0.0
readings.each { |r| sum += r.x }
p sum
d = ORIGIN + ORIGIN
p d.x, d.y

