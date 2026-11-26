# OpenSSL::HMAC.base64digest returns the MAC Base64-encoded, for each
# algorithm HMAC.digest takes
require "openssl"

puts OpenSSL::HMAC.base64digest("SHA256", "key", "data")
puts OpenSSL::HMAC.base64digest("sha1", "key", "data")
puts OpenSSL::HMAC.base64digest("SHA512", "key", "data")
puts OpenSSL::HMAC.base64digest(OpenSSL::Digest.new("SHA384"), "key", "")
p OpenSSL::HMAC.base64digest("SHA256", "k", "d") == [OpenSSL::HMAC.digest("SHA256", "k", "d")].pack("m0")
