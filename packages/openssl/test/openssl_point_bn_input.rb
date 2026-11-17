require "openssl"
group = OpenSSL::PKey::EC::Group.new("prime256v1")
point = OpenSSL::PKey::EC::Point.new(group, OpenSSL::BN.new("0"))
puts point.to_octet_string(:uncompressed).unpack1("H*")
class EncodedPoint < OpenSSL::BN
  def to_s(base = 10) = "overridden"
end
puts OpenSSL::PKey::EC::Point.new(group, EncodedPoint.new("0")).to_octet_string(:compressed).unpack1("H*")
# Published P-256 point, imported from PEM, then passed through a BN.
key = OpenSSL::PKey.read(<<~PEM)
  -----BEGIN PUBLIC KEY-----
  MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7
  Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==
  -----END PUBLIC KEY-----
PEM
bytes = key.public_key.to_octet_string(:uncompressed)
encoded = OpenSSL::BN.new(bytes, 2)
p OpenSSL::PKey::EC::Point.new(key.group, encoded).to_octet_string(:uncompressed) == bytes
