# spinel: not-cruby -- OpenSSL::PKey::EC.from_private_bytes is this package's; the .expected is the package's own answers
require "openssl"

def hx(s) = [s].pack("H*")

# RFC 8291 section 5 and appendix A. Every value here is published in the RFC,
# so nothing in this test is computed by the code under test.
#
# CEK: oIhVW04MRdy2XN9CiKLxTg   NONCE: 4h_95klXJ5E_qnoN
CEK   = hx("a088555b4e0c45dcb65cdf4288a2f14e")
NONCE = hx("e21ffde6495727913faa7a0d")
# "When I grow up, I want to be a watermelon" with the 0x02 padding delimiter
PT    = hx("5768656e20492067726f772075702c20492077616e7420746f20626520612077617465726d656c6f6e02")
# the RFC's emitted ciphertext, with its tag
CTTAG = hx("f297de5b429bba7153d3a4ae0caa091fd425f3b4b5414add8ab37a19c1bbb05cf5cb5b2a2e0562d558635641ec52812c6c8ff42e95ccb86be7cd")

c = OpenSSL::Cipher.new("aes-128-gcm")
c.encrypt
c.key = CEK
c.iv = NONCE
c.auth_data = ""
ct = c.update(PT) + c.final
p ct + c.auth_tag == CTTAG
p ct.bytesize == PT.bytesize          # GCM is a stream mode
p c.auth_tag.bytesize

d = OpenSSL::Cipher.new("aes-128-gcm")
d.decrypt
d.key = CEK
d.iv = NONCE
d.auth_data = ""
d.auth_tag = CTTAG.byteslice(CTTAG.bytesize - 16, 16)
p d.update(CTTAG.byteslice(0, CTTAG.bytesize - 16)) + d.final == PT

# The whole of RFC 8291 in one program: a keypair from its private bytes, the
# ECDH, the three HKDF steps, and the encryption -- ending at the body the RFC
# publishes in section 5.
AS_PRIV = hx("c9f58f89813e9f8e872e71f42aa64e1757c9254dcc62b72ddc010bb4043ea11c")
UA_PUB  = hx("042571b2becdfde360551aaf1ed0f4cd366c11cebe555f89bcb7b186a53339173168ece2ebe018597bd30479b86e3c8f8eced577ca59187e9246990db682008b0e")
AUTH    = hx("05305932a1c7eabe13b6cec9fda48882")
SALT    = hx("0c6bfaadad67958803092d454676f397")

as_key = OpenSSL::PKey::EC.from_private_bytes("prime256v1", AS_PRIV)
secret = as_key.dh_compute_key(UA_PUB)
ikm = OpenSSL::KDF.hkdf(secret, salt: AUTH,
                        info: "WebPush: info\0" + UA_PUB + as_key.public_key_bytes,
                        length: 32, hash: "SHA256")
cek = OpenSSL::KDF.hkdf(ikm, salt: SALT, info: "Content-Encoding: aes128gcm\0",
                        length: 16, hash: "SHA256")
nonce = OpenSSL::KDF.hkdf(ikm, salt: SALT, info: "Content-Encoding: nonce\0",
                          length: 12, hash: "SHA256")
p [cek == CEK, nonce == NONCE]

e = OpenSSL::Cipher.new("aes-128-gcm")
e.encrypt
e.key = cek
e.iv = nonce
e.auth_data = ""
body = e.update(PT) + e.final + e.auth_tag
p body == CTTAG

# aes-256-gcm, against the same operation run under CRuby's OpenSSL::Cipher.
K32  = hx("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
IV12 = hx("101112131415161718191a1b")
e2 = OpenSSL::Cipher.new("aes-256-gcm")
e2.encrypt
e2.key = K32
e2.iv = IV12
e2.auth_data = "hdr"
p (e2.update("payload") + e2.final + e2.auth_tag).unpack1("H*")

# The aad is authenticated but not emitted: "payload" is seven bytes in and
# seven bytes of ciphertext out, with the aad's only trace in the tag.
CT256  = hx("0d9fe17a26a85e")
TAG256 = hx("a987a03acf3f91ff25c44b825ee011ae")
d2 = OpenSSL::Cipher.new("aes-256-gcm")
d2.decrypt
d2.key = K32
d2.iv = IV12
d2.auth_data = "hdr"
d2.auth_tag = TAG256
p d2.update(CT256) + d2.final == "payload"

# An empty message still authenticates: the ciphertext is empty and the tag is
# not, which is the case a "did it produce anything?" check gets wrong.
e3 = OpenSSL::Cipher.new("aes-128-gcm")
e3.encrypt
e3.key = CEK
e3.iv = NONCE
empty_ct = e3.final
p [empty_ct.bytesize, e3.auth_tag.bytesize]
d3 = OpenSSL::Cipher.new("aes-128-gcm")
d3.decrypt
d3.key = CEK
d3.iv = NONCE
d3.auth_tag = e3.auth_tag
p d3.final == ""

# --- refusals ---

# A tampered ciphertext must not answer plaintext.
bad = OpenSSL::Cipher.new("aes-128-gcm")
bad.decrypt
bad.key = CEK
bad.iv = NONCE
bad.auth_tag = CTTAG.byteslice(CTTAG.bytesize - 16, 16)
flipped = CTTAG.byteslice(0, CTTAG.bytesize - 16)
flipped = hx("ff") + flipped.byteslice(1, flipped.bytesize - 1)
begin
  bad.update(flipped)
  bad.final
rescue OpenSSL::CipherError => ex
  puts "CipherError: #{ex.message}"
end

# Neither must a tampered aad, which is covered by the tag without appearing in
# the output.
bad2 = OpenSSL::Cipher.new("aes-256-gcm")
bad2.decrypt
bad2.key = K32
bad2.iv = IV12
bad2.auth_data = "hdrX"
bad2.auth_tag = TAG256
begin
  bad2.update(CT256)
  bad2.final
rescue OpenSSL::CipherError => ex
  puts "CipherError: #{ex.message}"
end

begin
  OpenSSL::Cipher.new("aes-256-cbc")
rescue OpenSSL::CipherError => ex
  puts "CipherError: #{ex.message}"
end

# a key or an iv of the wrong length is CRuby's ArgumentError
begin
  k = OpenSSL::Cipher.new("aes-128-gcm")
  k.encrypt
  k.key = hx("00" * 32)
rescue ArgumentError => ex
  puts "ArgumentError: #{ex.message}"
end
begin
  k2 = OpenSSL::Cipher.new("aes-128-gcm")
  k2.encrypt
  k2.iv = hx("00")
rescue ArgumentError => ex
  puts "ArgumentError: #{ex.message}"
end

begin
  t = OpenSSL::Cipher.new("aes-128-gcm")
  t.decrypt
  t.auth_tag = hx("0011")
rescue OpenSSL::CipherError => ex
  puts "CipherError: #{ex.message}"
end

begin
  n = OpenSSL::Cipher.new("aes-128-gcm")
  n.encrypt
  n.key = CEK
  n.iv = NONCE
  n.auth_tag
rescue OpenSSL::CipherError => ex
  puts "CipherError: #{ex.message}"
end

# A generated key and iv are the right widths and are not constant.
g1 = OpenSSL::Cipher.new("aes-256-gcm")
g2 = OpenSSL::Cipher.new("aes-256-gcm")
p [g1.random_key.bytesize, g1.random_iv.bytesize]
p g1.random_key != g2.random_key

p OpenSSL::CipherError.ancestors.take(3).map(&:to_s)
