require "openssl"

inner = OpenSSL::ASN1::Sequence.new([
  OpenSSL::ASN1::Integer.new(1), OpenSSL::ASN1::Integer.new(2)
])
value = OpenSSL::ASN1::Sequence.new(inner)
p value.value.equal?(inner)
puts inner.to_der.unpack1("H*")
puts value.to_der.unpack1("H*")
p value.to_der == inner.to_der
p value.map { |node| node.value }
puts OpenSSL::ASN1::Sequence.new([inner]).to_der.unpack1("H*")
empty = OpenSSL::ASN1::Sequence.new(OpenSSL::ASN1::Sequence.new([]))
puts empty.to_der.unpack1("H*")
class SyntheticSequence < OpenSSL::ASN1::Sequence
end
inherited = SyntheticSequence.new(inner)
p inherited.map { |node| node.value }
p inherited.to_der == inner.to_der
