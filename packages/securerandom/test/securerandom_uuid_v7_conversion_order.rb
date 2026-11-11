# spinel: int64
require "securerandom"
$events = +""
module Process
  def self.clock_gettime(clock, unit = :float_second)
    $events << "#{unit};"
    unit == :millisecond ? 123_456_789 : 123_456_789_654_321
  end
end
module SecureRandom
  def self.random_bytes(n = 16)
    $events << "random#{n};"
    "\xa1\xb2\xc3\xd4\xe5\xf6\x07\x18\x29\x3a".byteslice(0, n).dup
  end
end
class NumericBits < Numeric
  def to_int
    $events << "to_int;"
    6
  end
end
class TextBits < String
  def to_int
    $events << "text_to_int;"
    12
  end
end
class ParentBits
  def to_int
    $events << "parent_to_int;"
    9
  end
end
class InheritedBits < ParentBits
end
class FallbackBits
  def to_int
    $events << "to_int_nil;"
    nil
  end
  def to_i
    $events << "to_i;"
    2
  end
end
def check(label, value)
  $events = +""
  begin
    puts "#{label}: #{SecureRandom.uuid_v7(extra_timestamp_bits: value)} #{$events}"
  rescue => e
    puts "#{label}: #{e.class}: #{e.message} #{$events}"
  end
end
check("int", 4)
check("float", 4.9)
check("rational", Rational(49, 10))
check("numeric subclass", NumericBits.new)
check("string subclass override", TextBits.new("4"))
check("inherited", InheritedBits.new)
check("fallback", FallbackBits.new)
check("invalid", -1)
