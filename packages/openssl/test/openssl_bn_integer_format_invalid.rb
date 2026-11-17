require "openssl"
class Integer
  def to_s(base = 10) = "not-a-number"
end
bn = OpenSSL::BN.new("7")
begin
  puts bn.to_s == "7"
rescue => e
  puts e.class
end
begin
  puts (bn <=> 9) == -1
rescue => e
  puts e.class
end
puts bn == 7
