require "openssl"
group = OpenSSL::PKey::EC::Group.new("prime256v1")
point = OpenSSL::PKey::EC::Point.new(group)
begin
  point.send(:initialize, group, "invalid")
  puts "reinitialized"
rescue OpenSSL::PKey::EC::Point::Error => error
  puts error.message
end
begin
  puts point.to_octet_string(:uncompressed).unpack1("H*")
rescue OpenSSL::PKey::EC::Point::Error
  puts "point lost"
end
