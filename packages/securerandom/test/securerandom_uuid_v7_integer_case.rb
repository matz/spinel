# spinel: int64
require "securerandom"
module SecureRandom
  # Identity on these Integer arguments, avoiding the separate conversion fault.
  def self.Integer(value)
    value
  end
  def self.random_bytes(n = 16)
    "\x01\x23\x45\x67\x89\xab\xcd\xef\x10\x32".byteslice(0, n).dup
  end
end
module Process
  def self.clock_gettime(clock, unit = :float_second)
    unit == :millisecond ? 123_456_789 : 123_456_789_654_321
  end
end
big = 1 << 100
bits = big - (big - 12)
puts SecureRandom.uuid_v7(extra_timestamp_bits: bits)

# Preserve the 0 / 12 / range order even when arithmetic kept a Bignum slot.
[0, 1, 11, 12, 13].each do |n|
  value = big - (big - n)
  begin
    puts "#{n}: #{SecureRandom.uuid_v7(extra_timestamp_bits: value)}"
  rescue => error
    puts "#{n}: #{error.class}: #{error.message}"
  end
end
