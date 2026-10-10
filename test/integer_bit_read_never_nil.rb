# Integer#[] reads a bit and answers 0 or 1, never nil: a field written
# from `tmp[8]` (a carry) holds no nil bit. It was taken for an element read
# that can miss. A field never written before its read (@z) still holds nil.
class C
  def initialize; @a = 0; @c = 0; end
  def adc(d)
    tmp = @a + d + @c
    @c = tmp[8]
    @a = tmp & 0xff
    @z = @a[7]
  end
  attr_reader :a, :c, :z
end
x = C.new
x.adc(200); x.adc(100)
p x.a, x.c, x.z
p 5[0], 5[1], 6[1, 2], (2**70)[70], -1[100]
