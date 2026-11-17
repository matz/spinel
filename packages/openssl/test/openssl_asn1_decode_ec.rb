require "openssl"

# RFC 6979 A.2.5's fixed synthetic public test key.
key = OpenSSL::PKey.read(<<~PEM)
  -----BEGIN PUBLIC KEY-----
  MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7
  Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==
  -----END PUBLIC KEY-----
PEM
der = key.public_to_der
node = OpenSSL::ASN1.decode(der)
puts node.class
puts node.value.class
algorithm, point = node.value
puts algorithm.class
algorithm.value.each do |oid|
  puts oid.class, oid.value, oid.oid, oid.sn, oid.ln
end
puts point.class
puts point.value.unpack1("H*")
p [point.unused_bits, point.value.encoding == Encoding::BINARY]
p node.to_der == der
p node.value.map(&:tag)
p node.send(:to_der) == der
p OpenSSL::ASN1.send(:decode, der).to_der == der
p ::OpenSSL::ASN1.__send__(:decode, der).to_der == der

oid = OpenSSL::ASN1.decode(["06032a0304"].pack("H*"))
p [oid.value, oid.oid, oid.sn, oid.ln]
p oid.to_der.unpack1("H*")
