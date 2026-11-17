require "openssl"

[-32769, -32768, -257, -256, -255, -129, -128, -127, -1, 0, 1,
 127, 128, 129, 255, 256, 32767, 32768].each do |number|
  node = OpenSSL::ASN1::Integer.new(number)
  p [node.value, node.tag, node.tag_class, node.to_der.unpack1("H*")]
end

# A fixed ECDSA signature shape, including the sign-padding octet of r.
r = OpenSSL::BN.new("80ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 16)
s = OpenSSL::BN.new("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 16)
signature = OpenSSL::ASN1::Sequence.new([
  OpenSSL::ASN1::Integer.new(r), OpenSSL::ASN1::Integer.new(s)
])
puts signature.to_der.unpack1("H*")
p OpenSSL::ASN1::Integer.new(r).value.equal?(r)
p OpenSSL::ASN1::Integer.new(r).to_der == OpenSSL::ASN1::Integer.new(r.to_i).to_der
negative = OpenSSL::BN.new("-80ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 16)
puts OpenSSL::ASN1::Integer.new(negative).to_der.unpack1("H*")
puts OpenSSL::ASN1::Integer.new(negative.to_i).to_der.unpack1("H*")
p OpenSSL::ASN1::Integer.new(OpenSSL::BN.allocate).to_der.unpack1("H*")

class SyntheticInteger < OpenSSL::ASN1::Integer
  def value = "overridden"
end
p SyntheticInteger.new(128).to_der == OpenSSL::ASN1::Integer.new(128).to_der
puts SyntheticInteger.new(1).value
