require "openssl"

# Ruby OpenSSL accepts these definite-length BER forms and normalizes to DER.
["02810101", "0282000101", "028900000000000000000101", "030207ff", "030107"].each do |hex|
  node = OpenSSL::ASN1.decode([hex].pack("H*"))
  puts node.class
  if node.is_a?(OpenSSL::ASN1::Integer)
    puts node.value.class, node.value.to_s(16)
  elsif node.is_a?(OpenSSL::ASN1::Sequence)
    p node.value.length
  else
    p node.value.unpack1("H*"), node.unused_bits
  end
  puts node.to_der.unpack1("H*")
end
