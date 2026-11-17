require "openssl"
begin
  OpenSSL::PKey::EC::Point.new(OpenSSL::PKey::EC::Group.allocate)
rescue OpenSSL::OpenSSLError => error
  puts error.class
end
