require "openssl"

# Messages from libcrypto are deliberately excluded from the stable snapshot.
["", "02", "028201", "020201", "02820100", "02010100", "0200", "02020001",
 "0202ffff", "0300", "030208ff", "0600", "060180", "3003020201", "300102",
 "0480", "1f", "1f80", "1f0202"].each do |hex|
  begin
    OpenSSL::ASN1.decode([hex].pack("H*"))
    puts "unexpected success"
  rescue => error
    puts error.class
  end
end

class DerInput
  def to_der = "\x02\x01\x07".b
  def to_str = raise("to_str must not be called")
end
class StringInput
  private
  def to_str = "\x02\x01\x08".b
end
puts OpenSSL::ASN1.decode(DerInput.new).value.to_i
puts OpenSSL::ASN1.decode(StringInput.new).value.to_i
[nil, true, 1].each do |input|
  begin
    OpenSSL::ASN1.decode(input)
  rescue => error
    puts error.class
  end
end
