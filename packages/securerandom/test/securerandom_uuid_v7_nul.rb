# spinel: int64
require "securerandom"
begin
  SecureRandom.uuid_v7(extra_timestamp_bits: "3\0")
rescue => e
  puts "#{e.class}: #{e.message}"
end

["\0", "3\0tail", "3\0\n\"\\", "3\0".b, "bad\n\t\"\\",
 "9223372036854775808\0", "18446744073709551615\0", "18446744073709551616\0",
 "-9223372036854775809\0", "-18446744073709551615\0", "-18446744073709551616\0",
 "999999999999999999999999999999999\0"].each do |value|
  begin
    SecureRandom.uuid_v7(extra_timestamp_bits: value)
  rescue => error
    puts "#{error.class}: #{error.message}"
  end
end
