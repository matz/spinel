require "openssl"
number = OpenSSL::BN.new("123")
p number.to_s == "123"
p "value=#{number}" == "value=123"
