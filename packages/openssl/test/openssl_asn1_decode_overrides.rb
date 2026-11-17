require "openssl"

class OpenSSL::BN
  def initialize(*) = raise("BN initialize must not be called")
end
class OpenSSL::ASN1::Sequence
  def initialize(*) = raise("Sequence initialize must not be called")
end
class OpenSSL::ASN1::Integer
  def initialize(*) = raise("Integer initialize must not be called")
end
class OpenSSL::ASN1::ObjectId
  def initialize(*) = raise("ObjectId initialize must not be called")
end
class OpenSSL::ASN1::BitString
  def initialize(*) = raise("BitString initialize must not be called")
end
class Integer
  def to_s(*) = raise("Integer to_s must not be called")
end

begin
  seq = OpenSSL::ASN1.decode(["300d02010706032a030403030000ff"].pack("H*"))
  puts seq.class
  seq.value.each { |child| puts child.class }
  puts seq.value[0].value.to_s(16)
  p seq.to_der.unpack1("H*")
rescue => error
  puts error.class, error.message
end
