# spinel: int64
require "securerandom"
module Process
  def self.clock_gettime(clock, unit = :float_second)
    unit == :millisecond ? 123_456_789 : 123_456_789_654_321
  end
end
module SecureRandom
  def self.random_bytes(n = 16)
    "\x01\x23\x45\x67\x89\xab\xcd\xef\x10\x32".byteslice(0, n).dup
  end
end
class IntNil
  def to_int; nil; end
end
class IntString
  def to_int; "12"; end
end
class IntFloat
  def to_int; 12.0; end
end
class IntRaise
  def to_int; raise TypeError, "from to_int"; end
end
class IntBigZero
  def to_int; big = 1 << 100; big - big; end
end
class IntBigTwelve
  def to_int; big = 1 << 100; big - (big - 12); end
end
class IntBigThirteen
  def to_int; big = 1 << 100; big - (big - 13); end
end
class IntPrivate
  private
  def to_int; 12; end
end
def check(label, value)
  begin
    puts "#{label}: #{SecureRandom.uuid_v7(extra_timestamp_bits: value)}"
  rescue => e
    puts "#{label}: #{e.class}: #{e.message}"
  end
end
check("nil", nil)
check("to_int nil", IntNil.new)
check("to_int String", IntString.new)
check("to_int Float", IntFloat.new)
check("to_int raises", IntRaise.new)
check("to_int Bigint 0", IntBigZero.new)
check("to_int Bigint 12", IntBigTwelve.new)
check("to_int Bigint 13", IntBigThirteen.new)
check("private to_int", IntPrivate.new)
[-1.0, -0.999, -0.0, 0.999, 11.999, 12.999, 13.0, 1e100, -1e100,
 Float::INFINITY, -Float::INFINITY, Float::NAN].each { |n| check("Float #{n}", n) }
[Rational(-3, 2), Rational(-1, 2), Rational(25, 2), Rational(26, 2),
 Rational(1 << 100, 3), Rational(-(1 << 100), 3)].each { |n| check("Rational #{n}", n) }
