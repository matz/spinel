require 'openssl'
class Float
  def finite? = false
end
bits = OpenSSL::ASN1::BitString.new("\xff".b)
bits.unused_bits = 1.5
begin
  puts bits.to_der.unpack1('H*')
rescue => error
  puts error.class
end
