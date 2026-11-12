# spinel: int64
require "securerandom"
$uuid_ns = -1
module Process
  def self.clock_gettime(clock, unit = :float_second)
    unit == :millisecond ? $uuid_ns.div(1_000_000) : $uuid_ns
  end
end
module SecureRandom
  def self.random_bytes(n = 16)
    "\x01\x23\x45\x67\x89\xab\xcd\xef\x10\x32".byteslice(0, n).dup
  end
end
[-1, -999_999, -1_000_000, -1_000_001, -1_000_000_001, -1_700_000_000_123_456_789].each do |ns|
  $uuid_ns = ns
  (0..12).each do |bits|
    puts "#{ns}/#{bits}: #{SecureRandom.uuid_v7(extra_timestamp_bits: bits)}"
  end
end
