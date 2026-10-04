# CRuby's openssl extension requires digest, so a program that requires only
# "openssl" can name the top-level Digest, and it is not OpenSSL::Digest
# (#7242). aws-sigv4 requires only "openssl" and names Digest.
require "openssl"

p Digest
p OpenSSL::Digest
puts Digest::SHA256.hexdigest("abc") == OpenSSL::Digest::SHA256.hexdigest("abc")
puts Digest::SHA1.digest("abc") == OpenSSL::Digest::SHA1.digest("abc")
