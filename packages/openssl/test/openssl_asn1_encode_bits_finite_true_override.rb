require 'openssl'
class Float
  def finite? = true
end
[Float::INFINITY, -Float::INFINITY, Float::NAN].each do |n|
  bits = OpenSSL::ASN1::BitString.new('x')
  bits.unused_bits = n
  begin
    bits.to_der
  rescue => error
    puts error.class
  end
end
