# OpenSSL::HMAC with SHA1 runs on the SHA-1 block function the CPU's SHA
# instructions run where it has them: the one-shot HMAC.digest and
# HMAC.hexdigest, and an instance keyed once and fed in pieces. The answers
# are CRuby's: the RFC 2202 cases, a key longer than a block (hashed first),
# and every message length across the first two blocks after the key's.
require "openssl"

# Bytes with no pattern, the same under every Ruby.
def noise(n, seed)
  x = seed
  out = []
  n.times do
    x = (x * 75 + 74) % 65537
    out << (x % 256).chr
  end
  out.join
end

# RFC 2202, section 3.
cases = [
  ["\x0b" * 20, "Hi There"],
  ["Jefe", "what do ya want for nothing?"],
  ["\xaa" * 20, "\xdd" * 50],
  [(1..25).map(&:chr).join, "\xcd" * 50],
  ["\x0c" * 20, "Test With Truncation"],
  ["\xaa" * 80, "Test Using Larger Than Block-Size Key - Hash Key First"],
  ["\xaa" * 80, "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data"],
]
cases.each do |key, data|
  puts OpenSSL::HMAC.hexdigest("SHA1", key, data)
  puts OpenSSL::HMAC.digest("SHA1", key, data).unpack1("H*")
end

key = noise(20, 3)
long_key = noise(131, 5)
s = noise(140, 9)
(0..140).each do |n|
  puts "#{n} #{OpenSSL::HMAC.hexdigest("SHA1", key, s[0, n])} #{OpenSSL::HMAC.hexdigest("SHA1", long_key, s[0, n])}"
end

# Keyed once, fed in pieces that split blocks anywhere, reset between
# messages: each answer is the one-shot's.
big = noise(20_011, 15)
h = OpenSSL::HMAC.new(key, "SHA1")
[1, 13, 63, 64, 65, 1200].each do |step|
  h.reset
  i = 0
  while i < big.bytesize
    h.update(big[i, step])
    i += step
  end
  puts "#{step} #{h.hexdigest == OpenSSL::HMAC.hexdigest("SHA1", key, big)}"
end
puts OpenSSL::HMAC.hexdigest("SHA1", key, big)
