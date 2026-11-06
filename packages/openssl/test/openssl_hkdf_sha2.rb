require "openssl"

ikm = ("\x0b" * 22).freeze
salt = ["000102030405060708090a0b0c"].pack("H*").freeze
info = ["f0f1f2f3f4f5f6f7f8f9"].pack("H*").freeze
%w[SHA512 SHA384 SHA256].each do |name|
  hash = OpenSSL::Digest.new(name.downcase, "ignored")
  key = OpenSSL::KDF.hkdf(ikm, salt: salt, info: info, length: 82, hash: name.downcase)
  puts key.unpack1("H*")
  p OpenSSL::KDF.hkdf(ikm, salt: salt, info: info, length: 82, hash: hash) == key
  p OpenSSL::KDF.hkdf("wrong", salt: salt, info: info, length: 82, hash: hash) == key
  puts OpenSSL::KDF.hkdf("", salt: "", info: "", length: 1, hash: hash).unpack1("H*")
  p key.encoding == Encoding::BINARY
end

# Newly accepted Digest objects must hash their own input correctly too.
%w[SHA384 SHA512].each do |name|
  digest = OpenSSL::Digest.new(name)
  p digest.name == name
  digest.update("a\0".b)
  copy = digest.dup
  digest.update("b")
  p digest.digest == OpenSSL::Digest.digest(name, "a\0b".b)
  p digest.digest == OpenSSL::Digest.digest(name, "a\0b".b)
  p copy.digest == OpenSSL::Digest.digest(name, "a\0".b)
  p copy.digest == OpenSSL::Digest.digest(name, "a\0".b)
  p digest.reset.digest == OpenSSL::Digest.digest(name, "")
  puts digest.block_length
  digest.reset
  3.times { digest.update("\0a\xff".b * 97) }
  puts digest.hexdigest
end

begin
  OpenSSL::KDF.hkdf(ikm, salt: salt, info: info, length: 42, hash: "nonsense")
rescue => e
  puts "#{e.class}: #{e.message.downcase.split(/[(:]/).first.strip}"
end
