require "openssl"
begin
  OpenSSL::BN.new("255").to_s(8)
rescue ArgumentError => error
  puts error.class
  puts error.message
end
