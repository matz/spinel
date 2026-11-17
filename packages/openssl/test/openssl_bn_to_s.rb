require "openssl"
number = OpenSSL::PKey::EC::Group.new("prime256v1").order
p number.to_s == number.to_i.to_s
p number.to_s(16) == number.to_i.to_s(16).upcase
