require 'openssl'
wide = OpenSSL::BN.new('1361129467683753853853498429727072845824')
negative = OpenSSL::BN.new('-1361129467683753853853498429727072845824')
class Integer
  def to_s = '2'
end
puts OpenSSL::ASN1::Integer.new(1).to_der.unpack1('H*')
puts OpenSSL::ASN1::Integer.new(-128).to_der.unpack1('H*')
puts OpenSSL::ASN1::Integer.new(wide).to_der.unpack1('H*')
puts OpenSSL::ASN1::Integer.new(wide.to_i).to_der.unpack1('H*')
puts OpenSSL::ASN1::Integer.new(negative).to_der.unpack1('H*')
