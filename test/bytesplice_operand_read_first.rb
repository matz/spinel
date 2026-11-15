# The operands of String#bytesplice after the String value are read before
# the value's #to_str runs, as Ruby evaluates every argument before the
# call: an Array element, an instance variable or a global that #to_str
# changes is spliced with the value it had, whether the call is a
# statement or its value is kept.
# spinel: gc-stress

class Shift
  def initialize(a) = @a = a
  def to_str = (@a[0] = 1; $sh = 1; "XY")
end

idx = [0]
sa = +"abcdef"
sa.bytesplice(0, 1, Shift.new(idx), idx[0], 1)
p sa, idx

idx = [0]
sb = +"abcdef"
rb = sb.bytesplice(0, 1, Shift.new(idx), idx[0], 1)
p sb, rb.equal?(sb)

class Lengthen
  def initialize(a) = @a = a
  def to_str = (@a[1] = 2; "XY")
end
idx = [0, 1]
sl = +"abcdef"
sl.bytesplice(0, 1, Lengthen.new(idx), 0, idx[1])
p sl, idx

$sh = 0
sc = +"abcdef"
sc.bytesplice(0, 1, Shift.new([0]), $sh, 1)
p sc

class Ivn
  def initialize = (@n = 0; @s = +"abcdef")
  def to_str = (@n = 1; "XY")
  def go = (@s.bytesplice(0, 1, self, @n, 1); @s)
end
p Ivn.new.go

class LenShift
  def initialize = (@len = 1; @s = +"abcdef")
  def to_str = (@len = 2; "XY")
  def go = (@s.bytesplice(0, 1, self, 0, @len); @s)
end
p LenShift.new.go
