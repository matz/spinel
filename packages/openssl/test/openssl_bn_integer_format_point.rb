require "openssl"
class Integer
  def to_s(base = 10) = "1"
end
group = OpenSSL::PKey::EC::Group.new("prime256v1")
number = OpenSSL::BN.new("0")
begin
  point = OpenSSL::PKey::EC::Point.new(group, number)
  puts point.to_octet_string(:uncompressed).unpack1("H*")
rescue => error
  puts error.class
end
