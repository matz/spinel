require "openssl"
class Integer
  def to_s(base = 10) = "1"
end
bn = OpenSSL::BN.new("7")
puts bn.to_s
puts bn.to_s(16)
p(bn <=> OpenSSL::BN.new("9"))
p(bn <=> 9)
p(bn == 7)
