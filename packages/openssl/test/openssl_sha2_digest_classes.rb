require "openssl"

p OpenSSL::Digest::SHA384.hexdigest("abc")
p OpenSSL::Digest::SHA512.hexdigest("abc")
p OpenSSL::Digest::SHA512.digest("abc").bytesize
p OpenSSL::Digest::SHA384.base64digest("")
p OpenSSL::Digest::SHA384.superclass, OpenSSL::Digest::SHA512.superclass

d = OpenSSL::Digest::SHA512.new
d.update("a")
d.update("bc")
p d.hexdigest == OpenSSL::Digest::SHA512.hexdigest("abc")
p d.name, d.digest_length, d.block_length
p OpenSSL::Digest::SHA384.new("abc").hexdigest == OpenSSL::Digest::SHA384.hexdigest("abc")
p OpenSSL::Digest::SHA384.new.name

p OpenSSL::HMAC.hexdigest(OpenSSL::Digest::SHA512.new, "key", "data")
p OpenSSL::HMAC.hexdigest(OpenSSL::Digest::SHA384.new, "key", "data")
