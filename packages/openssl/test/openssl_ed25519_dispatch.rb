require "openssl"

class OpenSSL::PKey::PKey
  def raw_public_key = "x" * 32
end

key = OpenSSL::PKey.new_raw_private_key("ED25519", "s" * 32)
signature = key.sign(nil, "message")
p key.raw_public_key == "x" * 32
p key.verify(nil, signature, "message")
p key.verify(nil, signature, "changed")
p key.send(:verify, nil, signature, "message")

class CustomVerifier < OpenSSL::PKey::PKey
  def verify(digest, signature, data) = data == "custom"
end

custom = CustomVerifier.allocate
p custom.verify(nil, "", "custom")
p custom.verify(nil, "", "other")
