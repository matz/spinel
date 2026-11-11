# spinel: int64
require "securerandom"

big = 1 << 100
bits = big - (big - 12)
puts "bits=#{bits}"
begin
  puts "length=#{SecureRandom.uuid_v7(extra_timestamp_bits: bits).length}"
rescue => e
  puts "#{e.class}: #{e.message}"
end
