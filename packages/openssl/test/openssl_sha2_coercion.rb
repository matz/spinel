require "openssl"

# StringValue inputs for SHA2 Digest and one-shot HMAC paths.
# Coercion in Digest's older algorithm paths remains a separate follow-up.
class SHA2Input
  def initialize(text) = @text = text
  private def to_str = @text * 1
end
class SHA2String < String
  def to_s = "changed"
  def to_str = "changed"
  def dup = "changed"
  def bytesize = 0
  def b = "changed"
end

%w[SHA384 SHA512].each do |name|
  digest = OpenSSL::Digest.new(name, "before")
  [nil, 0].each do |value|
    begin
      digest.update(value)
    rescue => e
      puts "#{e.class}: #{e.message}"
    end
  end
  p digest.digest == OpenSSL::Digest.digest(name, "before")
  p digest.reset.update(SHA2Input.new("converted")).digest == OpenSSL::Digest.digest(name, "converted")
  p digest.reset.update(SHA2String.new("data\0").freeze).digest == OpenSSL::Digest.digest(name, "data\0")
  p digest.reset.update("é").update("\xff".b.freeze).digest == OpenSSL::Digest.digest(name, "é".b + "\xff".b)
end

key = SHA2String.new("key\0").freeze
message = SHA2String.new("data\0").freeze
%w[SHA384 SHA512].each do |name|
  expected = OpenSSL::HMAC.digest(name, "key\0", "data\0")
  p OpenSSL::HMAC.digest(name, key, message) == expected
  p OpenSSL::HMAC.digest(name, SHA2Input.new("key\0"), SHA2Input.new("data\0")) == expected
  digest = OpenSSL::Digest.new(name).freeze
  p OpenSSL::HMAC.digest(digest, key, message) == expected
end

begin
  OpenSSL::HMAC.digest("SHA512", nil, "data")
rescue => e
  puts "#{e.class}: #{e.message}"
end
begin
  OpenSSL::HMAC.digest("SHA384", "key", nil)
rescue => e
  puts "#{e.class}: #{e.message}"
end
