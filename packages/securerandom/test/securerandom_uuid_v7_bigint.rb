# spinel: int64
require "securerandom"
begin
  SecureRandom.uuid_v7(extra_timestamp_bits: 1 << 100)
rescue => e
  puts "#{e.class}: #{e.message}"
end
[1e100, Rational(1 << 100, 3), Complex(1e100, 0)].each do |value|
  begin
    SecureRandom.uuid_v7(extra_timestamp_bits: value)
  rescue => error
    puts "#{error.class}: #{error.message}"
  end
end
