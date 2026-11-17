require "openssl"

class SyntheticNodeList
  private
  def to_a
    [OpenSSL::ASN1::Integer.new(3)]
  end
end
class SyntheticDerString
  private
  def to_str = "\x02\x01\x01".b
end
class SyntheticDerChild
  def to_der = "\x02\x01\x02".b
end
class SyntheticBadNodeList
  private
  def to_a = "invalid"
end
puts OpenSSL::ASN1::Sequence.new(SyntheticNodeList.new).to_der.unpack1("H*")
puts OpenSSL::ASN1::Sequence.new([
  "\x02\x01\x00".b, SyntheticDerString.new, SyntheticDerChild.new
]).to_der.unpack1("H*")
begin
  OpenSSL::ASN1::Sequence.new(SyntheticBadNodeList.new).to_der
rescue => error
  puts error.class
end
