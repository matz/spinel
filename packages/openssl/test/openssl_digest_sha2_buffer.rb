require "openssl"

chunk = (("a\0\xff".b * 5461) + "!").freeze # 16 KiB, including binary bytes.
input = chunk * 64 # 1 MiB.
["SHA384", "SHA512", "SHA256"].each do |name|
  digest = OpenSSL::Digest.new(name)
  32.times { digest.update(chunk) }
  copied = digest.dup
  cloned = digest.clone
  32.times { digest.update(chunk) }
  expected = OpenSSL::Digest.digest(name, input)
  p digest.digest == expected
  copied.update("dup\0".b)
  cloned.update("clone\xff".b)
  p copied.digest == OpenSSL::Digest.digest(name, chunk * 32 + "dup\0".b)
  p cloned.digest == OpenSSL::Digest.digest(name, chunk * 32 + "clone\xff".b)
  p digest.digest == expected
  p digest.digest == expected
  p copied.reset.update(chunk).digest == OpenSSL::Digest.digest(name, chunk)
  p cloned.reset.digest == OpenSSL::Digest.digest(name, "")
  p digest.digest == expected
end
