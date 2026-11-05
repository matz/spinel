require "openssl"

# RFC 8032, section 7.1, test 1: https://www.rfc-editor.org/rfc/rfc8032#section-7.1
seed = ["9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"].pack("H*")
expected = ["e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f" \
            "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"].pack("H*")
key = OpenSSL::PKey.new_raw_private_key("ED25519", seed).freeze
seed.replace("\0" * 32)
puts key.class
signature = key.sign(nil, "")
puts signature.bytesize
p signature == expected
p OpenSSL::PKey.new_raw_private_key("ed25519", "s" * 32).sign(nil, "data\0bytes").bytesize == 64
begin
  key.sign("SHA256", "message")
rescue OpenSSL::PKey::PKeyError => error
  puts error.class
end
begin
  key.sign("SHA224", "message")
rescue OpenSSL::PKey::PKeyError => error
  puts error.class
end
begin
  key.sign("unknown", "message")
rescue OpenSSL::Digest::DigestError => error
  puts error.class
end
begin
  key.sign("SHA256\0suffix", "message")
rescue ArgumentError => error
  puts error.class
end

class Ed25519Seed
  def to_str = "s" * 32
end

class Ed25519Data
  def to_str = "data\0bytes"
end

class Ed25519SeedString < String
  def dup = "x" * 32
  def bytesize = 1
end

normal = OpenSSL::PKey.new_raw_private_key("ED25519", "s" * 32)
subclass = OpenSSL::PKey.new_raw_private_key("ED25519", Ed25519SeedString.new("s" * 32))
p normal.sign(nil, "message") == subclass.sign(nil, "message")

p OpenSSL::PKey.new_raw_private_key("ED25519", Ed25519Seed.new).sign(nil, Ed25519Data.new).bytesize == 64
[nil, Object.new].each do |invalid|
  begin
    OpenSSL::PKey.new_raw_private_key("ED25519", invalid)
  rescue TypeError => error
    puts error.class
  end
  begin
    key.sign(nil, invalid)
  rescue TypeError => error
    puts error.class
  end
  begin
    key.sign("SHA256", invalid)
  rescue TypeError => error
    puts error.class
  end
end
begin
  OpenSSL::PKey::PKey.new
rescue TypeError => error
  puts error.class
end
begin
  OpenSSL::PKey::PKey.allocate.sign(nil, "")
rescue RuntimeError => error
  puts error.class
end
begin
  OpenSSL::PKey.new_raw_private_key("ED25519\0suffix", "s" * 32)
rescue ArgumentError => error
  puts error.class
end
["", "s" * 31, "s" * 33].each do |invalid|
  begin
    OpenSSL::PKey.new_raw_private_key("ED25519", invalid)
  rescue OpenSSL::PKey::PKeyError => error
    puts error.class
  end
end
begin
  OpenSSL::PKey.new_raw_private_key("NO_SUCH_ALGORITHM", "s" * 32)
rescue OpenSSL::PKey::PKeyError => error
  puts error.class
end
