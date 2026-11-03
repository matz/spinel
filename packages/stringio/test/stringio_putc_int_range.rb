# spinel: int64 -- the arguments below pass 2^31
# StringIO#putc of an Integer past a C int raises RangeError
# CRuby's putc converts as NUM2CHR, through a C int: 2147483648 (or the
# Float) is "integer 2147483648 too big to convert to 'int'". putc wrote the
# low byte instead, typed or boxed. A Complex whose real part is past a long
# converts to a Bignum, which does not fit either. The stream's write check
# still comes first.
require "stringio"

def t(label)
  p [label, :ok, yield]
rescue IOError, RangeError => e
  p [label, e.class, e.message]
end

big = 2147483648
io = StringIO.new
nums = [1, big, 2147483648.0, -2147483649, 2147483647, -2147483648,
        Complex(2147483648.0, 0), Complex(1e30, 0), Complex(-1e30, 0)]
nums.each_with_index { |_, i| t(:"n#{i}") { io.putc(nums[i + ARGV.size]) } }
t(:typed_big) { io.putc(big) }
t(:typed_max) { io.putc(2147483647) }
p io.string.bytes
t(:big_on_frozen) { StringIO.new("a".freeze).putc(big) }
