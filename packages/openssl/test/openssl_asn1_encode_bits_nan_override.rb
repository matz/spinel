require 'openssl'
class Float
  def nan? = true
end
bits = OpenSSL::ASN1::BitString.new('x')
bits.unused_bits = Float::INFINITY
begin
  bits.to_der
rescue => error
  puts error.class
  puts error.message
end
