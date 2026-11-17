require "openssl"
group = OpenSSL::PKey::EC::Group.new("prime256v1")
point = OpenSSL::PKey::EC::Point.new(group)
copy = OpenSSL::PKey::EC::Point.new(point)
puts copy.to_octet_string(:compressed).unpack1("H*")
