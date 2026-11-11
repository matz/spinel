# spinel: int64
require "securerandom"

def selected_bits(first)
  first ? 0 : (1 << 100)
end

bits = selected_bits(ARGV.empty?)
puts "bits=#{bits}"
begin
  puts "length=#{SecureRandom.uuid_v7(extra_timestamp_bits: bits).length}"
rescue => e
  puts "#{e.class}: #{e.message}"
end

begin
  SecureRandom.uuid_v7(extra_timestamp_bits: selected_bits(false))
rescue => e
  puts "#{e.class}: #{e.message}"
end
