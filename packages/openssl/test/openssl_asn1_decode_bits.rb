require "openssl"

["", "\x00", "\xff\x80", "abc" * 100].each do |bytes|
  8.times do |unused|
    original = OpenSSL::ASN1::BitString.new(bytes.b)
    original.unused_bits = unused
    der = original.to_der
    node = OpenSSL::ASN1.decode(der)
    puts node.class
    p [node.value.unpack1("H*"), node.unused_bits, node.tag, node.tag_class]
    p node.value.encoding == Encoding::BINARY
    p node.to_der == der
  end
end

# A decoded value owns its bytes, independent of the input and other calls.
der = ["03030000ff"].pack("H*")
first = OpenSSL::ASN1.decode(der.freeze)
second = OpenSSL::ASN1.decode(der)
first.value.replace("changed")
p second.to_der == der
p first.to_der != der
