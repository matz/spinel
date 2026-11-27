# Digest::SHA1 takes its input a 64-byte block at a time, on the CPU's SHA
# instructions where it has them and in portable C where it does not. Every
# answer here is CRuby's: the FIPS 180 examples, every length across the
# first two blocks (so every shape of the padding, which fits beside the
# data up to 55 bytes into a block and takes a block of its own from 56),
# inputs long enough to stay in the block loop, the same bytes fed in
# pieces, and a digest of digests.
require "digest"

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

puts Digest::SHA1.hexdigest("")
puts Digest::SHA1.hexdigest("abc")
puts Digest::SHA1.hexdigest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
puts Digest::SHA1.hexdigest("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")
puts Digest::SHA1.hexdigest("a" * 1_000_000)

s = noise(130, 9)
(0..130).each { |n| puts Digest::SHA1.hexdigest(s[0, n]) }

big = noise(100_003, 15)
puts Digest::SHA1.hexdigest(big)
puts Digest::SHA1.base64digest(big)
puts Digest::SHA1.digest(big).unpack1("H*")
puts Digest::SHA1.hexdigest("\x00" * 4099)
puts Digest::SHA1.hexdigest("\xff" * 4099)

# The same bytes fed in pieces that split blocks anywhere: the state carried
# between them is the chaining value the block function leaves.
[1, 7, 55, 56, 63, 64, 65, 119, 120, 1000].each do |step|
  d = Digest::SHA1.new
  i = 0
  while i < big.bytesize
    d << big[i, step]
    i += step
  end
  puts "#{step} #{d.hexdigest == Digest::SHA1.hexdigest(big)}"
end

# Two digests and a byte side by side are 41 bytes, and their digest the
# next pair's half: each round's state is the one before it, 200 times over.
d = Digest::SHA1.digest("")
200.times { d = Digest::SHA1.digest(d + "." + d) }
puts d.unpack1("H*")
