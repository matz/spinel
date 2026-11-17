require "openssl"

class Float
  def to_int = raise("Float#to_int must not be called")
end
class Integer
  def to_int = raise("Integer#to_int must not be called")
end
[-0.9, 1.9, 7.9, 0, 1, 7].each do |value|
  bits = OpenSSL::ASN1::BitString.new("\xff".b)
  bits.unused_bits = value
  puts bits.to_der.unpack1("H*")
end
[2147483647.9, 2147483648.0, -2147483648.9, -2147483649.0,
 1.2345678912345e30, -9223372036854775808.0, 9223372036854775808.0,
 OpenSSL::BN.new("2147483648").to_i, OpenSSL::BN.new("-2147483649").to_i,
 OpenSSL::BN.new("1267650600228229401496703205376").to_i].each do |value|
  bits = OpenSSL::ASN1::BitString.new("x")
  bits.unused_bits = value
  begin
    bits.to_der
  rescue => error
    puts error.class
    puts error.message
  end
end

class SyntheticBitCount
  attr_reader :calls
  def initialize
    @calls = 0
  end
  private
  def to_int
    @calls += 1
    2
  end
end
count = SyntheticBitCount.new
bits = OpenSSL::ASN1::BitString.new("\xff".b)
bits.unused_bits = count
puts bits.to_der.unpack1("H*")
p count.calls
p bits.unused_bits.equal?(count)
