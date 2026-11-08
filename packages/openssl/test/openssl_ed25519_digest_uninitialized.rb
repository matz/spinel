require "openssl"

key = OpenSSL::PKey.new_raw_private_key("ED25519", "s" * 32)
signature = key.sign(nil, "message")

begin
  p key.verify(OpenSSL::Digest.allocate, signature, "message")
rescue => error
  p error.class
  p error.message == "Digest CTX wasn't initialized!"
end

begin
  p key.verify(OpenSSL::Digest.allocate, nil, nil)
rescue => error
  p error.class
  p error.message == "Digest CTX wasn't initialized!"
end
