require "openssl"

class SignatureBytes
  def initialize(signature, digest)
    @signature = signature
    @digest = digest
  end

  def to_str
    @digest.name.clear
    @signature
  end
end

class MessageBytes
  def initialize(digest)
    @digest = digest
  end

  def to_str
    @digest.name.clear
    "message"
  end
end

key = OpenSSL::PKey.new_raw_private_key("ED25519", "s" * 32)
signature = key.sign(nil, "message")
digest = OpenSSL::Digest.new("SHA256")
begin
  p key.verify(digest, SignatureBytes.new(signature, digest), "message")
rescue => error
  p error.class
  p error.message.start_with?("EVP_DigestVerifyInit:")
end

digest = OpenSSL::Digest.new("SHA256")
begin
  p key.verify(digest, signature, MessageBytes.new(digest))
rescue => error
  p error.class
  p error.message.start_with?("EVP_DigestVerifyInit:")
end
