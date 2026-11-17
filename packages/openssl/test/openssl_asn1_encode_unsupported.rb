# spinel: not-cruby -- This encoding subset refuses OID equality and tagged constructors.
# CRuby provides these APIs; explicit rejection prevents unrelated Object behavior.
require "openssl"
first = OpenSSL::ASN1::ObjectId.new("SHA256")
second = OpenSSL::ASN1::ObjectId.new("sha256")
begin
  first == second
rescue NoMethodError
  puts "equality unavailable"
end
begin
  first.send(:==, second)
rescue NoMethodError
  puts "send equality unavailable"
end
begin
  first.__send__(:==, second)
rescue NoMethodError
  puts "__send__ equality unavailable"
end
begin
  OpenSSL::ASN1::Sequence.new([], 0)
rescue NoMethodError
  puts "Sequence tagging unavailable"
end
begin
  OpenSSL::ASN1::ObjectId.new("SHA256", 0)
rescue NoMethodError
  puts "ObjectId tagging unavailable"
end
begin
  OpenSSL::ASN1::BitString.new("", 0)
rescue NoMethodError
  puts "BitString tagging unavailable"
end
begin
  OpenSSL::ASN1::Integer.new(1, 0)
rescue NoMethodError
  puts "Integer tagging unavailable"
end
