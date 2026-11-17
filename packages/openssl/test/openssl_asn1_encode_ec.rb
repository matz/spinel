require "openssl"

algorithm = OpenSSL::ASN1::Sequence.new([
  OpenSSL::ASN1::ObjectId.new("id-ecPublicKey"),
  OpenSSL::ASN1::ObjectId.new("prime256v1")
])
puts algorithm.to_der.unpack1("H*")

# RFC 6979 A.2.5's fixed synthetic public test key.
key = OpenSSL::PKey.read(<<~PEM)
  -----BEGIN PUBLIC KEY-----
  MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7
  Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==
  -----END PUBLIC KEY-----
PEM
point = key.public_key.to_octet_string(:uncompressed)
bits = OpenSSL::ASN1::BitString.new(point)
public_key = OpenSSL::ASN1::Sequence.new([algorithm, bits]).freeze
p public_key.to_der == key.public_to_der
puts public_key.to_der.unpack1("H*")
p public_key.value[0].equal?(algorithm)
p bits.value.equal?(point)
p [algorithm.tag, bits.tag, public_key.tag_class, bits.unused_bits]
p public_key.to_der.encoding == Encoding::BINARY
copy = public_key.to_der
copy.replace("changed")
p public_key.to_der == key.public_to_der
p public_key.send(:to_der) == public_key.__send__(:to_der)

class SyntheticSequence < OpenSSL::ASN1::Sequence
  def to_der = "overridden"
end
puts SyntheticSequence.new([]).to_der
p OpenSSL::ASN1::Sequence.new([SyntheticSequence.new([])]).to_der.unpack1("H*")
