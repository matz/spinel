# Setting the iv (or the key) starts a new message, as CRuby's EVP re-init
# does: the input updates gave before it is not part of the message final
# seals, and a cipher used up by final takes a new message the same way.
require "openssl"

key = "k" * 16
iv1 = "a" * 12
iv2 = "b" * 12

c = OpenSSL::Cipher.new("aes-128-gcm").encrypt
c.key = key
c.iv = iv1
first = c.update("hello")
c.iv = iv2
second = c.update("world")
rest = c.final
tag = c.auth_tag
p [first.bytesize, second.bytesize, rest.bytesize]

d = OpenSSL::Cipher.new("aes-128-gcm").decrypt
d.key = key
d.iv = iv2
d.auth_tag = tag
p d.update(second) + d.final

c.iv = iv1
again = c.update("again") + c.final
tag2 = c.auth_tag
e = OpenSSL::Cipher.new("aes-128-gcm").decrypt
e.key = key
e.iv = iv1
e.auth_tag = tag2
p e.update(again) + e.final
