require "openssl"
class Integer
  def to_s(base = 10) = "1"
end
puts 7.to_s == "1"
bn = OpenSSL::BN.new("7")
puts bn.to_s == "7"
puts bn.to_s(16) == "07"
puts (bn <=> OpenSSL::BN.new("9")) == -1
puts (bn <=> 9) == -1
puts bn == OpenSSL::BN.new("7")
puts bn == 7
