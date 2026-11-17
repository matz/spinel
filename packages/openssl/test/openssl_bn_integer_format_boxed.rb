require 'openssl'
class Integer
  def to_s(base = 10) = 'not-a-number'
end
[7, -7, -9223372036854775808, 18446744073709551617, -18446744073709551617].each do |value|
  number=OpenSSL::BN.new(value).freeze
  puts number.to_s
  puts number.to_s(16)
  p(number <=> value)
  p(number <=> OpenSSL::BN.new(value))
  GC.start
end
group=OpenSSL::PKey::EC::Group.new('prime256v1')
puts OpenSSL::PKey::EC::Point.new(group,OpenSSL::BN.new(0)).to_octet_string(:uncompressed).unpack1('H*')
