require "openssl"

# RFC 6979 A.2.5's synthetic P-256 public test key.
key = OpenSSL::PKey.read(<<~PEM)
  -----BEGIN PUBLIC KEY-----
  MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7
  Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==
  -----END PUBLIC KEY-----
PEM
puts key.group.order.class
puts key.group.order.to_i.class
puts key.group.order.to_i.to_s(16)
point = key.public_key.freeze
puts point.class
[:uncompressed, :compressed, :hybrid].each do |form|
  bytes = point.to_octet_string(form)
  puts bytes.bytesize
  puts bytes.unpack1("H*")
  p bytes.encoding == Encoding::BINARY
  bytes.replace("changed")
  p point.to_octet_string(form) != bytes
end

# The public-only key gives the same point, independently of later calls.
public_key = OpenSSL::PKey.read(key.public_to_der).freeze
p public_key.public_key.to_octet_string(:compressed) == point.to_octet_string(:compressed)
first = point.to_octet_string(:uncompressed)
other = OpenSSL::PKey.read(<<~PEM)
  -----BEGIN PUBLIC KEY-----
  MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEqofKIr6LBTeOscce8yCtdG4dO2KLp5uY
  WfdB4IJUKjhVAvJdv1UpbDpUXjhydgq3NhfeSpYmLG9dnpi/kpLcKfj0Hb0omhR8
  6doxE7XwuMAKYLHOHX6BnXpDHXyQ6g5f
  -----END PUBLIC KEY-----
PEM
puts other.group.order.to_i.to_s(16)
puts other.public_key.to_octet_string(:uncompressed).bytesize
puts other.public_key.to_octet_string(:compressed).bytesize
p first == point.to_octet_string(:uncompressed)
copy = point
chosen = true ? copy : public_key.public_key
p chosen.to_octet_string(:compressed) == point.to_octet_string(:compressed)

[:bogus, "compressed", nil, 4, Object.new].each do |form|
  begin
    point.to_octet_string(form)
  rescue ArgumentError => error
    puts error.class
  end
end

# Native parsing accepts each libcrypto encoding and the point at infinity.
group = key.group.freeze
[:compressed, :uncompressed, :hybrid].each do |form|
  encoded = point.to_octet_string(form)
  restored = OpenSSL::PKey::EC::Point.new(group, encoded)
  encoded.replace("changed")
  p restored.to_octet_string(:uncompressed) == first
end
puts OpenSSL::PKey::EC::Point.new(group).to_octet_string(:uncompressed).unpack1("H*")
["", "broken", "\x04".b + "\0" * 64].each do |invalid|
  begin
    OpenSSL::PKey::EC::Point.new(group, invalid)
  rescue OpenSSL::PKey::EC::Point::Error => error
    puts error.class
  end
end
begin
  OpenSSL::PKey::EC::Group.allocate.order
rescue OpenSSL::PKey::EC::Group::Error => error
  puts error.class
end
begin
  OpenSSL::PKey::EC::Point.allocate.to_octet_string(:uncompressed)
rescue OpenSSL::PKey::EC::Point::Error => error
  puts error.class
end

class NamedGroup < OpenSSL::PKey::EC::Group
  def curve_name = "overridden"
end
p OpenSSL::PKey::EC::Point.new(NamedGroup.new("prime256v1"), first).to_octet_string(:uncompressed) == first
class NamedPoint < OpenSSL::PKey::EC::Point
  def to_octet_string(form) = "overridden"
end
puts NamedPoint.new(group, first).to_octet_string(:compressed)
puts ::OpenSSL::PKey::EC::Group.new("prime256v1").order.to_i.to_s(16)
p point.public_send(:to_octet_string, [:compressed].first) == point.to_octet_string(:compressed)
