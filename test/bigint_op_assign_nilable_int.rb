# spinel: int64
# A Bignum local's `op=` takes an Integer operand that can be nil
# `n += s.getbyte(i)` on a Bignum local unwrapped the operand to an sp_int
# and passed it where sp_bigint_add takes a Bignum: the C did not build.
def add_byte(s)
  n = 2**70
  n += s.getbyte(0)
  n
end
p add_byte("\x01")

def sub_first(a)
  n = 2**70
  n -= a.first
  n
end
p sub_first([3])

def mul_index(a)
  n = 2**70
  n *= a.index(7)
  n
end
p mul_index([5, 6, 7])

def or_byte(s)
  n = 2**70
  n |= s.getbyte(0)
  n
end
p or_byte("\x05")

def through_local(s)
  n = 2**70
  x = s.getbyte(0)
  n += x
  n
end
p through_local("\x02")

def bytes_to_i(s)
  n = 0
  i = 0
  while i < s.bytesize
    n = n * 256
    n += s.getbyte(i)
    i += 1
  end
  n
end
p bytes_to_i("\x01" * 12)

class Acc
  def initialize
    @n = 2**70
  end

  def add(s)
    @n += s.getbyte(0)
    @n
  end
end
p Acc.new.add("\x03")

def nil_operand(s)
  n = 2**70
  n += s.getbyte(9)
  n
end
begin
  nil_operand("a")
rescue TypeError => e
  p e.message
end
