require "openssl"

class DigestName < String
  def nil? = true
end

class DigestLike
  def is_a?(klass) = true
end

key = OpenSSL::PKey.new_raw_private_key("ED25519", "s" * 32)
signature = key.sign(nil, "message")

begin
  p key.verify(DigestName.new("SHA256"), signature, "message")
rescue => error
  p error.class
  p error.message.start_with?("EVP_DigestVerifyInit:")
end

begin
  p key.verify(DigestLike.new, signature, "message")
rescue => error
  p error.class
  p error.message.include?("DigestLike")
end
