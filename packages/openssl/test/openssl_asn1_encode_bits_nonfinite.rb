require 'openssl'
[Float::INFINITY, -Float::INFINITY, Float::NAN].each do |n|
  bits = OpenSSL::ASN1::BitString.new('x')
  bits.unused_bits = n
  begin
    bits.to_der
  rescue => e
    puts e.class
    puts e.message
  end
end
