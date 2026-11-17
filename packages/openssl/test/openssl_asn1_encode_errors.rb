require "openssl"

invalid = OpenSSL::ASN1::ObjectId.new("synthetic-unknown-object-id")
puts "construction defers validation"
begin
  invalid.to_der
rescue => error
  puts error.class
  p error.is_a?(OpenSSL::OpenSSLError)
end
begin
  invalid.oid
rescue => error
  puts error.class
end
begin
  OpenSSL::ASN1::ObjectId.new("SHA256\0ignored").sn
rescue => error
  puts error.class
end
[-1, 8, nil, "1", 2147483648].each do |unused|
  node = OpenSSL::ASN1::BitString.new("\xff".b)
  node.unused_bits = unused
  puts "setter accepts value"
  begin
    node.to_der
  rescue => error
    puts error.class
  end
end
[nil, "128", 1.5, Object.new].each do |value|
  begin
    OpenSSL::ASN1::Integer.new(value).to_der
  rescue => error
    puts error.class
  end
end
begin
  OpenSSL::ASN1::BitString.new(nil).to_der
rescue => error
  puts error.class
end
begin
  OpenSSL::ASN1::ObjectId.new(nil).oid
rescue => error
  puts error.class
end
puts OpenSSL::ASN1::Sequence.new(nil).to_der.unpack1("H*")
begin
  OpenSSL::ASN1::Sequence.new(Object.new).to_der
rescue => error
  puts error.class
end
