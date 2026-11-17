require "openssl"

# Values stay exact even in the default machine-integer arithmetic mode.
["0", "7", "-255", "18446744073709551617", "-18446744073709551617"].each do |decimal|
  number = OpenSSL::BN.new(decimal).freeze
  puts number.to_i
  puts OpenSSL::BN.new(number).to_i
end
["", "\0", "\x01\0\xff".b, "\xff".b * 32].each do |bytes|
  puts OpenSSL::BN.new(bytes, 2).to_i.to_s(16)
end
["FF", "-FF", "123junk", "0xff"].each do |hex|
  puts OpenSSL::BN.new(hex, 16).to_i
end
[0, 7, -7, 18446744073709551617].each do |integer|
  p OpenSSL::BN.new(integer).to_i == integer
end
puts OpenSSL::BN.allocate.to_i
bytes = "123".dup
number = OpenSSL::BN.new(bytes)
bytes.replace("999")
puts number.to_i
puts OpenSSL::BN.new(["000000020180"].pack("H*"), 0).to_i
["", "junk"].each do |invalid|
  begin
    OpenSSL::BN.new(invalid)
  rescue OpenSSL::BNError => error
    puts error.class
  end
end
["1\0suffix", "FF\0suffix"].each do |invalid|
  begin
    OpenSSL::BN.new(invalid, 16)
  rescue ArgumentError => error
    puts error.class
  end
end
begin
  OpenSSL::BN.new("1", 8)
rescue ArgumentError => error
  puts error.class
end
[nil, Object.new].each do |invalid|
  begin
    OpenSSL::BN.new(invalid)
  rescue TypeError, ArgumentError => error
    puts error.class
  end
end

class BNBytes
  def to_str = "123"
end
class BNBase
  def to_int = 16
end
class OtherBN < OpenSSL::BN
  def to_i = 999
end
puts OpenSSL::BN.new(BNBytes.new).to_i
puts OpenSSL::BN.new("ff", BNBase.new).to_i
puts OpenSSL::BN.new("12", 2.0).to_i
puts OpenSSL::BN.new(OtherBN.new("7")).to_i
[nil, "2", Object.new].each do |base|
  begin
    OpenSSL::BN.new("12", base)
  rescue TypeError => error
    puts error.class
  end
end
begin
  OpenSSL::BN.new("1", 2147483648)
rescue RangeError => error
  puts error.class
end
