# OpenSSL::HMAC.new(key, "SHA256") hashes the padded key once; a `dup` of
# the keyed object, fed and read, answers the MAC the one-shot function
# answers, whatever the key's length (none, under a block, a block, over a
# block and so hashed down), the message's (across block edges, in pieces),
# and however often the object is read, reset or fed again after a read.
# A chain of a thousand MACs, each over the one before, keyed once, is the
# signing loop this is for.
require "openssl"

def noise(n, seed)
  x = seed
  out = []
  n.times do
    x = (x * 75 + 74) % 65537
    out << (x % 256).chr
  end
  out.join
end

msg = noise(200, 9)
["SHA256", "SHA1"].each do |algo|
  [0, 1, 32, 63, 64, 65, 131].each do |klen|
    key = noise(klen, 5)
    keyed = OpenSSL::HMAC.new(key, algo)
    same = [0, 1, 31, 55, 56, 63, 64, 65, 119, 120, 128, 200].all? do |n|
      keyed.dup.update(msg[0, n]).digest == OpenSSL::HMAC.digest(algo, key, msg[0, n])
    end
    puts "#{algo} key #{klen}: #{same} #{keyed.dup.update(msg).hexdigest}"
  end
end

key = noise(32, 3)
h = OpenSSL::HMAC.new(key, "SHA256")
h << msg[0, 10] << msg[10, 70] << msg[80, 120]
puts h.hexdigest == OpenSSL::HMAC.hexdigest("SHA256", key, msg)
puts h.hexdigest == h.hexdigest
h.update("more")
puts h.hexdigest == OpenSSL::HMAC.hexdigest("SHA256", key, msg + "more")
puts h.reset.hexdigest == OpenSSL::HMAC.hexdigest("SHA256", key, "")
puts h.inspect == h.hexdigest
puts h.base64digest == [OpenSSL::HMAC.digest("SHA256", key, "")].pack("m0")

# a copy fed after the original is fed again does not see it, nor the other way
a = OpenSSL::HMAC.new(key, "SHA256").update("ab")
b = a.dup
a.update("c")
b.update("d")
puts a.hexdigest == OpenSSL::HMAC.hexdigest("SHA256", key, "abc")
puts b.hexdigest == OpenSSL::HMAC.hexdigest("SHA256", key, "abd")

base = OpenSSL::HMAC.new(key, "SHA256")
v = "1700000000:42".b
w = v
1000.times { v = base.dup.update(v).digest }
1000.times { w = OpenSSL::HMAC.digest("SHA256", key, w) }
puts v == w
puts v.unpack1("H*")

# SHA384 keeps its data; the same reads answer the same
k384 = OpenSSL::HMAC.new(key, "SHA384")
puts k384.dup.update(msg).hexdigest == OpenSSL::HMAC.hexdigest("SHA384", key, msg)
