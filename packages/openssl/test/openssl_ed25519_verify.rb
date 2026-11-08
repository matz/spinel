require "openssl"

# RFC 8032 section 7.1, test 1 (empty message).
# https://www.rfc-editor.org/rfc/rfc8032#section-7.1
seed = ["9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"].pack("H*")
signature = ["e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f" \
             "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"].pack("H*")
key = OpenSSL::PKey.new_raw_private_key("ED25519", seed).freeze
seed.replace("\0" * 32)
p key.class
p key.sign(nil, "") == signature
p key.verify(nil, signature, "")
p key.verify(nil, signature, "changed")
changed_signature = signature.dup
changed_signature.setbyte(0, changed_signature.getbyte(0) ^ 1)
p key.verify(nil, changed_signature, "")
["", signature.byteslice(0, 63), signature + "\0"].each do |bad_signature|
  p key.verify(nil, bad_signature, "")
end
other = OpenSSL::PKey.new_raw_private_key("ED25519", "t" * 32)
p other.verify(nil, signature, "")
message = "binary\0message\xff".b
p key.verify(nil, key.sign(nil, message), message)

class Ed25519Bytes
  def initialize(bytes) = @bytes = bytes
  def to_str = @bytes
end

class Ed25519Signature < String
  def bytesize = 1
  def to_s = "wrong"
end

p key.verify(nil, Ed25519Bytes.new(signature), Ed25519Bytes.new(""))
p key.verify(nil, Ed25519Signature.new(signature), "")
[nil, Object.new].each do |invalid|
  begin
    key.verify(nil, invalid, "")
  rescue => error
    p error.class
  end
  begin
    key.verify(nil, signature, invalid)
  rescue => error
    p error.class
  end
  begin
    key.verify("SHA256", invalid, invalid)
  rescue => error
    p error.class
  end
end
begin
  key.verify("SHA256", signature, "")
rescue => error
  p error.class
end
begin
  key.verify(OpenSSL::Digest.new("SHA256"), signature, "")
rescue => error
  p error.class
end
begin
  key.verify("SHA256\0tail", signature, "")
rescue => error
  p error.class
end
begin
  OpenSSL::PKey::PKey.allocate.verify(nil, signature, "")
rescue => error
  p error.class
end
begin
  OpenSSL::PKey.new_raw_private_key("NO_SUCH_ALGORITHM", "s" * 32)
rescue => error
  p error.class
end
