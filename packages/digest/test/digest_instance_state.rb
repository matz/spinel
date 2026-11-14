# Digest::SHA256.new (and SHA1, MD5) keeps the running hash: fed in pieces
# of every size across block edges it answers the one-shot digest, reading
# it changes nothing, and a copy taken after a block (the padded key of an
# HMAC written by hand) starts from there. A thousand MACs chained that way
# answer what OpenSSL::HMAC answers.
require "digest"

def noise(n, seed)
  x = seed
  out = []
  n.times do
    x = (x * 75 + 74) % 65537
    out << (x % 256).chr
  end
  out.join
end

msg = noise(300, 9) + "\0\0" + noise(40, 4)
[[:SHA256, Digest::SHA256], [:SHA1, Digest::SHA1], [:MD5, Digest::MD5]].each do |name, klass|
  ok = [1, 7, 55, 56, 63, 64, 65, 129].all? do |step|
    d = klass.new
    i = 0
    while i < msg.bytesize
      d << msg.byteslice(i, step)
      i += step
    end
    d.hexdigest == klass.hexdigest(msg) && d.digest == klass.digest(msg) && d.hexdigest == d.hexdigest
  end
  e = klass.new.update(msg)
  puts "#{name}: #{ok} #{e.hexdigest} #{e.base64digest == klass.base64digest(msg)}"
  puts "#{name} reset: #{e.reset.hexdigest == klass.hexdigest("")}"
end

# HMAC-SHA256 by hand: the key's inner and outer blocks hashed once, copied per message
key = noise(32, 3)
padded = key.b.ljust(64, "\0")
inner = Digest::SHA256.new.update(padded.bytes.map { |b| b ^ 0x36 }.pack("C*"))
outer = Digest::SHA256.new.update(padded.bytes.map { |b| b ^ 0x5c }.pack("C*"))
v = "1700000000:42".b
1000.times { v = outer.dup.update(inner.dup.update(v).digest).digest }
puts v.unpack1("H*")
puts inner.hexdigest == Digest::SHA256.hexdigest(padded.bytes.map { |b| b ^ 0x36 }.pack("C*"))
