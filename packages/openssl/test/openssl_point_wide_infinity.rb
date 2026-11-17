require "openssl"
group = OpenSSL::PKey::EC::Group.new("sect571r1")
point = OpenSSL::PKey::EC::Point.new(group)
puts point.to_octet_string(:compressed).unpack1("H*")
