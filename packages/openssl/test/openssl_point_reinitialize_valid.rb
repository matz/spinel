require "openssl"
group = OpenSSL::PKey::EC::Group.new("prime256v1")
point = OpenSSL::PKey::EC::Point.new(group)
begin
  point.send(:initialize, group, "\0".b)
  puts "accepted"
rescue OpenSSL::PKey::EC::Point::Error
  puts "already initialized"
end
puts point.to_octet_string(:uncompressed).unpack1("H*")
