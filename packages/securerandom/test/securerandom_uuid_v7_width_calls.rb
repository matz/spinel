# spinel: int64
require "securerandom"
$calls = +""
module Process
  def self.clock_gettime(clock, unit = :float_second)
    $calls << "clock:#{unit};"
    unit == :millisecond ? 123_456_789 : 123_456_789_654_321
  end
end
module SecureRandom
  def self.random_bytes(n = 16)
    $calls << "bytes:#{n};"
    "\x01\x23\x45\x67\x89\xab\xcd\xef\x10\x32".byteslice(0, n).dup
  end
end
(-1..13).each do |bits|
  $calls = +""
  begin
    puts "#{bits}: #{SecureRandom.uuid_v7(extra_timestamp_bits: bits)}"
  rescue => e
    puts "#{bits}: #{e.class}: #{e.message}"
  end
  puts "calls=#{$calls}"
end
