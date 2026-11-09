# OpenSSL::HMAC compares by its MAC, as CRuby does: two objects with the
# same key, algorithm and data are equal, and anything that is not an HMAC
# (the hex String of the MAC included) is not. #inspect shows the MAC, not
# the key or the data. #base64digest is the MAC in Base64.
require "openssl"
h = OpenSSL::HMAC.new("key", "SHA256")
g = OpenSSL::HMAC.new("key", "SHA256")
p h == g
h << "a"
p h == g, h != g
g << "a"
p h == g
p h == OpenSSL::HMAC.new("other", "SHA256").update("a")
p h == OpenSSL::HMAC.new("key", "SHA1").update("a")
p h == h.hexdigest, h == 1, h == nil
p h.inspect
p h.inspect == h.hexdigest
p h.base64digest
p h.base64digest == [h.digest].pack("m0")
p [g].include?(h)
p OpenSSL::HMAC.new("key", "SHA512") == OpenSSL::HMAC.new("key", "SHA384")
s = Struct.new(:m).new(OpenSSL::HMAC.new("k\0z", "SHA256"))
p s
D = Data.define(:m)
p D.new(m: OpenSSL::HMAC.new("k", "SHA256"))
class LoudH < OpenSSL::HMAC; def hexdigest = "LOUD"; end
l = LoudH.new("key", "SHA256"); l << "a"; p l
