require "openssl"

point = "\xff\xff".b
node = OpenSSL::ASN1::BitString.new(point)
p [node.value.equal?(point), node.tag, node.tag_class, node.unused_bits]
(0..7).each do |bits|
  node.unused_bits = bits
  p [node.unused_bits, node.to_der.unpack1("H*")]
end
p point.unpack1("H*")
puts OpenSSL::ASN1::BitString.new("\x80\x00".b).to_der.unpack1("H*")
node = OpenSSL::ASN1::BitString.new("")
node.unused_bits = 7
puts node.to_der.unpack1("H*")

class SyntheticUnusedBits
  private
  def to_int = 3
end
node = OpenSSL::ASN1::BitString.new("\xff".b)
converted = SyntheticUnusedBits.new
node.unused_bits = converted
p node.unused_bits.equal?(converted)
puts node.to_der.unpack1("H*")
node.unused_bits = 1.9
puts node.to_der.unpack1("H*")
node.freeze
puts node.to_der.unpack1("H*")
begin
  node.unused_bits = 2
rescue => error
  puts error.class
end
