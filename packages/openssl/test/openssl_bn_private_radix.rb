require "openssl"
class Radix
  private
  def to_int = 16
end
puts OpenSSL::BN.new("ff", Radix.new).to_i
