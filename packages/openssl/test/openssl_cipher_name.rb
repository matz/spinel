# OpenSSL::Cipher#name returns OpenSSL's name for the cipher, whatever
# spelling was passed to new
require "openssl"

p OpenSSL::Cipher.new("aes-256-gcm").name
p OpenSSL::Cipher.new("AES-256-GCM").name
p OpenSSL::Cipher.new("Aes-128-Gcm").name
c = OpenSSL::Cipher.new("aes-128-gcm")
c.encrypt
p c.name
p c.key_len
