require "openssl"

class HMACInput
  def initialize(text) = @text = text
  private def to_str = @text * 1
end

class HMACStringOnly
  def to_s = "converted"
end

class HMACString < String
  def to_s = "changed"
  def to_str = "changed"
end

text = "converted\0\xff".b.freeze
inputs = [1, :sym, nil, HMACInput.new(text), HMACStringOnly.new, HMACString.new(text).freeze]
%w[SHA1 SHA256 SHA384 SHA512].each do |name|
  puts name
  inputs.each do |value|
    begin
      hmac = OpenSSL::HMAC.new(value, name)
      p hmac.update("payload").digest == OpenSSL::HMAC.digest(name, text, "payload")
    rescue => e
      puts "#{e.class}: #{e.message}"
    end
  end

  inputs.each do |value|
    hmac = OpenSSL::HMAC.new("key", name).update("prefix")
    begin
      p hmac.update(value).digest == OpenSSL::HMAC.digest(name, "key", "prefix".b + text)
    rescue => e
      puts "#{e.class}: #{e.message}"
      p hmac.digest == OpenSSL::HMAC.digest(name, "key", "prefix")
    end
  end
end
