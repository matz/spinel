require "openssl"

sequence = OpenSSL::ASN1::Sequence.new([])
sequence.send(:initialize, [OpenSSL::ASN1::Integer.new(3)])
puts sequence.to_der.unpack1("H*")
oid = OpenSSL::ASN1::ObjectId.new("SHA256")
oid.send(:initialize, "prime256v1")
p [oid.value, oid.oid]
bits = OpenSSL::ASN1::BitString.new("\xff".b)
bits.unused_bits = 7
bits.send(:initialize, "\x80".b)
p bits.unused_bits
puts bits.to_der.unpack1("H*")
integer = OpenSSL::ASN1::Integer.new(1)
integer.send(:initialize, -128)
puts integer.to_der.unpack1("H*")
begin
  integer.freeze.send(:initialize, 2)
rescue => error
  puts error.class
end
begin
  OpenSSL::ASN1::ASN1Data.new(nil, 1, "UNIVERSAL")
rescue => error
  puts error.class
end
