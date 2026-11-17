require "openssl"
group = OpenSSL::PKey::EC::Group.new("prime256v1")
number = group.order
p number == group.order
p number == OpenSSL::BN.new(number)
p number == number.to_i
p number == OpenSSL::BN.new("1")
[7, 7.0, "7", nil, Object.new, OpenSSL::BN.new("7")].each do |other|
  p OpenSSL::BN.new("7") == other
end
