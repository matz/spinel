# spinel: not-cruby -- exercises package-only EC.from_private_bytes and raw public-point APIs
require "openssl"

def hx(s) = [s].pack("H*")

# RFC 8291 section 5 and appendix A: a complete Web Push example with fixed
# keys, so every value below is published rather than computed here. The
# base64url beside each is the RFC's own spelling of the same bytes.
#
# as_private: yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw
AS_PRIV = hx("c9f58f89813e9f8e872e71f42aa64e1757c9254dcc62b72ddc010bb4043ea11c")
# as_public: BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8
AS_PUB = hx("04fe33f4ab0dea71914db55823f73b54948f41306d920732dbb9a59a53286482200e597a7b7bc260ba1c227998580992e93973002f3012a28ae8f06bbb78e5ec0f")
# ua_private: q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94
UA_PRIV = hx("ab5757a70dd4a53e553a6bbf71ffefea2874ec07a6b379e3c48f895a02dc33de")
# ua_public: BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4
UA_PUB = hx("042571b2becdfde360551aaf1ed0f4cd366c11cebe555f89bcb7b186a53339173168ece2ebe018597bd30479b86e3c8f8eced577ca59187e9246990db682008b0e")
# ecdh_secret: kyrL1jIIOHEzg3sM2ZWRHDRB62YACZhhSlknJ672kSs
SECRET = hx("932acbd63208387133837b0cd995911c3441eb66000998614a592727aef6912b")
# auth_secret: BTBZMqHH6r4Tts7J_aSIgg  /  salt: DGv6ra1nlYgDCS1FRnbzlw
AUTH = hx("05305932a1c7eabe13b6cec9fda48882")
SALT = hx("0c6bfaadad67958803092d454676f397")
# IKM: S4lYMb_L0FxCeq0WhDx813KgSYqU26kOyzWUdsXYyrg
IKM = hx("4b895831bfcbd05c427aad16843c7cd772a0498a94dba90ecb359476c5d8cab8")
# CEK: oIhVW04MRdy2XN9CiKLxTg  /  NONCE: 4h_95klXJ5E_qnoN
CEK = hx("a088555b4e0c45dcb65cdf4288a2f14e")
NONCE = hx("e21ffde6495727913faa7a0d")

as_key = OpenSSL::PKey::EC.from_private_bytes("prime256v1", AS_PRIV)
ua_key = OpenSSL::PKey::EC.from_private_bytes("prime256v1", UA_PRIV)

# The public half derives from the private one; both are the RFC's.
p as_key.public_key_bytes == AS_PUB
p ua_key.public_key_bytes == UA_PUB
p as_key.private_key_bytes == AS_PRIV

# ECDH agrees in both directions, and on the RFC's published secret.
p as_key.dh_compute_key(UA_PUB) == SECRET
p ua_key.dh_compute_key(AS_PUB) == SECRET

# The rest of the RFC 8291 key schedule, which is HKDF three times.
key_info = "WebPush: info\0" + UA_PUB + AS_PUB
ikm = OpenSSL::KDF.hkdf(SECRET, salt: AUTH, info: key_info, length: 32, hash: "SHA256")
p ikm == IKM
p OpenSSL::KDF.hkdf(ikm, salt: SALT, info: "Content-Encoding: aes128gcm\0", length: 16, hash: "SHA256") == CEK
p OpenSSL::KDF.hkdf(ikm, salt: SALT, info: "Content-Encoding: nonce\0", length: 12, hash: "SHA256") == NONCE

# RFC 5869 test case 1, so HKDF is pinned by its own vector and not only by a
# caller's.
p OpenSSL::KDF.hkdf(hx("0b" * 22), salt: hx("000102030405060708090a0b0c"),
                    info: hx("f0f1f2f3f4f5f6f7f8f9"), length: 42,
                    hash: "SHA256").unpack1("H*")

# A generated key is a usable key: the two sides of a fresh exchange agree.
a = OpenSSL::PKey::EC.generate("prime256v1")
b = OpenSSL::PKey::EC.generate("prime256v1")
p a.private_key_bytes.bytesize
p a.public_key_bytes.bytesize
p a.public_key_bytes.getbyte(0)
p a.private_key_bytes != b.private_key_bytes
p a.dh_compute_key(b.public_key_bytes) == b.dh_compute_key(a.public_key_bytes)

# The lengths follow the curve, not a constant: P-384 is 48 and 97.
c = OpenSSL::PKey::EC.generate("secp384r1")
p [c.private_key_bytes.bytesize, c.public_key_bytes.bytesize]
p c.dh_compute_key(OpenSSL::PKey::EC.generate("secp384r1").public_key_bytes).bytesize

# Refusals. An off-curve peer point is the one that matters: an ECDH against
# it leaks the private scalar to whoever chose it, so it must not answer.
bad = AS_PUB.byteslice(0, 64) + "\xff".b
begin
  ua_key.dh_compute_key(bad)
rescue OpenSSL::PKey::ECError => e
  puts "ECError: #{e.message}"
end

begin
  OpenSSL::PKey::EC.generate("brainpoolP256r1x")
rescue OpenSSL::PKey::ECError => e
  puts "ECError: #{e.message}"
end

# A scalar of the wrong width is not a key on this curve.
begin
  OpenSSL::PKey::EC.from_private_bytes("prime256v1", "\x01".b * 31)
rescue OpenSSL::PKey::ECError => e
  puts "ECError: #{e.message}"
end

# Zero is a scalar of the right width and still not a key.
begin
  OpenSSL::PKey::EC.from_private_bytes("prime256v1", "\0" * 32)
rescue OpenSSL::PKey::ECError => e
  puts "ECError: #{e.message}"
end

begin
  OpenSSL::KDF.hkdf("ikm", salt: "", info: "", length: 32, hash: "MD5")
rescue OpenSSL::OpenSSLError => e
  puts "#{e.class}: #{e.message}"
end

begin
  OpenSSL::KDF.hkdf("ikm", salt: "", info: "", length: 8161, hash: "SHA256")
rescue OpenSSL::OpenSSLError => e
  puts "#{e.class}: #{e.message}"
end

# ECError is an alias of PKeyError in CRuby 4.0.7.
p OpenSSL::PKey::ECError.ancestors.take(4).map(&:to_s)
