require "openssl"

[-(2**255 + 7), -(2**64 + 1), -129, -128, -1, 0, 1, 127, 128, 255, 256,
 2**63, 2**64 + 1, 2**255 + 7].each do |number|
  der = OpenSSL::ASN1::Integer.new(number).to_der
  node = OpenSSL::ASN1.decode(der)
  puts node.class
  puts node.value.class
  p node.value.to_i == number
  p node.value.to_s(16)
  p [node.tag, node.tag_class, node.to_der == der]
end

sig = OpenSSL::ASN1::Sequence.new([
  OpenSSL::ASN1::Integer.new(2**255 + 7), OpenSSL::ASN1::Integer.new(5)
]).to_der
seq = OpenSSL::ASN1.decode(sig)
r, s = seq.value.map(&:value)
puts r.class, r.to_i.to_s(16), s.to_i
p seq.to_der == sig
p OpenSSL::ASN1.decode(OpenSSL::ASN1::Sequence.new([])).value.empty?
