# spinel: gc-stress
# Array.new(n, fill) with a fill the operand builds where it stands (an
# empty Array or Hash, an interpolated String, a new object) held the fill
# only in a C temp while the array was allocated: under SPINEL_GC_STRESS=2
# that allocation freed it before the first push, and every slot held the
# freed object.
class Pt
  def initialize(x) = (@x = x)
  def to_s = "Pt(#{@x})"
end
a = Array.new(2, [])
a[0] << 1
p a
p Array.new(2, {}), Array.new(1, {k: [1]})
i = 7
p Array.new(3, "x#{i}"), Array.new(2, "y" * i)
p Array.new(2, Pt.new(i)).map(&:to_s)
p Array.new(3, 5), Array.new(2, nil), Array.new(0, "z#{i}")
