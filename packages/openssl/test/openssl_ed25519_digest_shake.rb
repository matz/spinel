require "openssl"

key = OpenSSL::PKey.new_raw_private_key("ED25519", "s" * 32)
signature = key.sign(nil, "message")

["SHAKE128", "SHAKE256"].each do |digest|
  begin
    p key.verify(digest, signature, "message")
  rescue => error
    p error.class
    p error.message.start_with?("EVP_DigestVerifyInit:")
  end
  begin
    p key.verify(digest, nil, "message")
  rescue => error
    p error.class
    p error.message == "no implicit conversion of nil into String"
  end
end
