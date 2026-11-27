# AES-CTR through OpenSSL::Cipher: the published vectors, updates of any
# size, and CRuby's answers for what a counter-mode cipher does not have.
require "openssl"

def hex(s) = s.unpack1("H*")
def bin(h) = [h].pack("H*")

def ctr(name, key, iv, encrypt: true)
  c = OpenSSL::Cipher.new(name)
  encrypt ? c.encrypt : c.decrypt
  c.key = key
  c.iv = iv
  c
end

# NIST SP 800-38A, F.5.1, F.5.3 and F.5.5 (CTR-AES*.Encrypt)
plain = bin("6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51" \
            "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710")
counter = bin("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
[
  ["aes-128-ctr", "2b7e151628aed2a6abf7158809cf4f3c",
   "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff" \
   "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee"],
  ["aes-192-ctr", "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b",
   "1abc932417521ca24f2b0459fe7e6e0b090339ec0aa6faefd5ccc2c6f4ce8e94" \
   "1e36b26bd1ebc670d1bd1d665620abf74f78a7f6d29809585a97daec58c6b050"],
  ["aes-256-ctr", "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
   "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5" \
   "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"],
].each do |name, key, want|
  c = ctr(name, bin(key), counter)
  out = c.update(plain) + c.final
  puts "#{name} sp800-38a #{hex(out) == want ? "ok" : "MISMATCH #{hex(out)}"}"
  d = ctr(name, bin(key), counter, encrypt: false)
  puts "#{name} decrypt #{d.update(out) + d.final == plain ? "ok" : "MISMATCH"}"
  c = OpenSSL::Cipher.new(name)
  puts "#{name} name=#{c.name} key_len=#{c.key_len} iv_len=#{c.iv_len}"
end

# RFC 3711 B.2: the AES-CM keystream, at the first blocks and at the last
# ones of the vector, reached by an update that starts far into the message
key = bin("2b7e151628aed2a6abf7158809cf4f3c")
iv = bin("f0f1f2f3f4f5f6f7f8f9fafbfcfd0000")
c = ctr("aes-128-ctr", key, iv)
stream = c.update("\0" * 48)
puts "rfc3711 b.2 head #{hex(stream) == "e03ead0935c95e80e166b16dd92b4eb4" \
  "d23513162b02d0f72a43a2fe4a5f97ab41e95b3bb0a2e8dd477901e4fca894c0" ? "ok" : "MISMATCH #{hex(stream)}"}"
c.update("\0" * (0xfeff * 16 - 48))
tail = c.update("\0" * 48)
puts "rfc3711 b.2 tail #{hex(tail) == "ec8cdf7398607cb0f2d21675ea9ea1e4" \
  "362b7c3c6773516318a077d7fc5073ae6a2cc3787889374fbeb4c81b17ba6c44" ? "ok" : "MISMATCH #{hex(tail)}"}"

# RFC 3711 B.3: session keys from the master key, labels 0, 1 and 2
master = bin("e1f97a0d3e018be0d64fa32c06de4139")
salt = bin("0ec675ad498afeebb6960b3aabe6")
[[0, 16, "c61e7a93744f39ee10734afe3ff7a087"],
 [1, 20, "cebe321f6ff7716b6fd4ab49af256a156d38baa4"],
 [2, 14, "30cbbc08863d8c85d49db34a9ae1"]].each do |label, length, want|
  x = salt.bytes
  x[7] ^= label
  k = ctr("aes-128-ctr", master, x.pack("C*") + "\0\0").update("\0" * length)
  puts "rfc3711 b.3 label #{label} #{hex(k) == want ? "ok" : "MISMATCH #{hex(k)}"}"
end

# The counter is all 128 bits: it wraps from ff..ff to 00..00
wrap = ctr("aes-128-ctr", key, "\xff".b * 16).update("\0" * 48)
zero = ctr("aes-128-ctr", key, "\0" * 16).update("\0" * 32)
first = ctr("aes-128-ctr", key, "\xff".b * 16).update("\0" * 16)
puts "wrap #{wrap == first + zero ? "ok" : "MISMATCH"} #{hex(wrap)}"
carry = ctr("aes-128-ctr", key, bin("00000000000000ffffffffffffffffff")).update("\0" * 32)
puts "carry #{hex(carry)}"

# Updates of any size answer what one update of the whole does
data = (0...300).map { |i| (i * 37 + 11) % 256 }.pack("C*")
whole = ctr("aes-256-ctr", "K" * 32, "I" * 16).update(data)
[[1, 15, 16, 17, 0, 31, 33, 48, 7], [5] * 60, [299, 1], [16] * 19].each do |sizes|
  c = ctr("aes-256-ctr", "K" * 32, "I" * 16)
  pieces = +"".b
  at = 0
  sizes.each do |n|
    piece = c.update(data.byteslice(at, n) || "")
    pieces << piece
    at += n
  end
  pieces << c.update(data.byteslice(at, data.bytesize - at)) if at < data.bytesize
  puts "chunks #{sizes.first(4).inspect} #{pieces == whole ? "ok" : "MISMATCH"}"
end
puts "whole #{hex(whole[0, 24])} #{whole.encoding}"

# final answers "" and the stream goes on after it; iv= starts it again
c = ctr("aes-128-ctr", "k" * 16, "i" * 16)
a = c.update("abcd")
f = c.final
b = c.update("efgh")
puts "final #{f.inspect} #{f.encoding} continue #{hex(a + b)}"
c.iv = "i" * 16
puts "iv restart #{hex(c.update("abcdefgh"))}"

# key=, encrypt and decrypt re-initialise: the counter carries on from the
# next whole block, and an update needs the key set again after a direction
c = ctr("aes-128-ctr", "k" * 16, "i" * 16)
c.update("abcd")
c.key = "k" * 16
puts "key reinit #{hex(c.update("efgh"))}"
c = ctr("aes-128-ctr", "k" * 16, "i" * 16)
c.update("ab")
c.decrypt
begin
  c.update("cd")
rescue OpenSSL::Cipher::CipherError => e
  puts "after decrypt: #{e.class}: #{e.message}"
end
c.key = "k" * 16
puts "decrypt then key #{hex(c.update("cd"))}"
c.iv = "i" * 16
c.encrypt
c.key = "k" * 16
puts "iv then encrypt then key #{hex(c.update("abcd"))}"

# No iv set is an iv of zeros; no key set raises
c = OpenSSL::Cipher.new("aes-128-ctr")
c.key = "k" * 16
z = ctr("aes-128-ctr", "k" * 16, "\0" * 16)
puts "zero iv #{c.update("abcd") == z.update("abcd") ? "ok" : "MISMATCH"}"
begin
  OpenSSL::Cipher.new("aes-128-ctr").encrypt.update("x")
rescue OpenSSL::Cipher::CipherError => e
  puts "no key: #{e.message}"
end

# What a counter-mode cipher has no use for, as CRuby answers it
c = ctr("aes-128-ctr", "k" * 16, "i" * 16)
[-> { c.auth_data = "x" }, -> { c.auth_tag }, -> { c.auth_tag = "t" * 16 },
 -> { c.iv_len = 12 }, -> { c.iv = "i" * 12 }, -> { c.key = "k" * 15 },
 -> { c.update(nil) }, -> { c.update(5) }].each do |op|
  op.call
  puts "no error"
rescue StandardError => e
  puts "#{e.class}: #{e.message}"
end
empty = c.update("")
puts "empty update #{empty.inspect} #{empty.encoding}"
puts "random_key #{c.random_key.bytesize} random_iv #{c.random_iv.bytesize}"
begin
  OpenSSL::Cipher.new("aes-512-ctr")
rescue OpenSSL::Cipher::CipherError => e
  puts "aes-512-ctr: #{e.class}"
end
