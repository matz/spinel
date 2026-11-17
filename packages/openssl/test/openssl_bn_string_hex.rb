require "openssl"
number = OpenSSL::BN.new("255")
puts number.to_s(16)
["0", "1", "15", "-1", "255"].each do |decimal|
  number = OpenSSL::BN.new(decimal).freeze
  puts number.to_s(16)
  puts number.to_s(2).unpack1("H*")
  puts number.to_s(0).unpack1("H*")
end
