# spinel: int64
require "securerandom"

module Process
  def self.clock_gettime(clock, unit = :float_second)
    unit == :millisecond ? 1_700_000_000_123 : 1_700_000_000_123_456_789
  end
end
module SecureRandom
  def self.random_bytes(n = 16)
    "\xa5" * n
  end
end

class V7ToInt
  def to_int; 7; end
end
class V7ToI
  def to_i; 3; end
end
class V7Fallback
  def to_int; nil; end
  def to_i; 4; end
end
class V7BadInt
  def to_int; "3"; end
end
class V7Raises
  def to_int; raise TypeError, "conversion failed"; end
end

def check_v7_argument(label, value)
  begin
    puts "#{label}: #{SecureRandom.uuid_v7(extra_timestamp_bits: value)}"
  rescue => error
    puts "#{label}: #{error.class}: #{error.message}"
  end
end

check_v7_argument("integer 0", 0)
check_v7_argument("integer 12", 12)
check_v7_argument("float 1.5", 1.5)
check_v7_argument("float 0.9", 0.9)
check_v7_argument("float -0.9", -0.9)
check_v7_argument("float 12.9", 12.9)
check_v7_argument("string decimal", "3")
check_v7_argument("string hex", "0xc")
check_v7_argument("string whitespace", " 4 ")
check_v7_argument("string binary", "0b10")
check_v7_argument("to_int", V7ToInt.new)
check_v7_argument("to_i", V7ToI.new)
check_v7_argument("fallback", V7Fallback.new)
check_v7_argument("rational", Rational(7, 2))
check_v7_argument("complex real", Complex(4, 0))
check_v7_argument("complex imaginary", Complex(1, 1))
check_v7_argument("negative", -1)
check_v7_argument("too large", 13)
check_v7_argument("float below", -1.5)
check_v7_argument("float above", 13.1)
check_v7_argument("nil", nil)
check_v7_argument("true", true)
check_v7_argument("false", false)
check_v7_argument("symbol", :bits)
check_v7_argument("invalid string", "x")
check_v7_argument("fractional string", "1.5")
check_v7_argument("object", Object.new)
check_v7_argument("bad to_int", V7BadInt.new)
check_v7_argument("conversion raises", V7Raises.new)
check_v7_argument("infinity", Float::INFINITY)
check_v7_argument("nan", Float::NAN)

# Equivalent call spellings and keyword forwarding use the same default.
puts ::SecureRandom.uuid_v7
puts SecureRandom.uuid_v7(**{extra_timestamp_bits: 12})
puts SecureRandom.uuid_v7(**{})
