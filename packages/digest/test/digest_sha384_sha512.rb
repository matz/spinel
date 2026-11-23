# Digest::SHA384 and Digest::SHA512 (and Digest::SHA2 via digest/sha2):
# NIST FIPS 180-4 test vectors, every length across two 128-byte blocks,
# multi-chunk streaming splits, dup/clone independence, reset, file, ==,
# Digest.hexencode, and full Digest::Instance method parity with CRuby 4.0.
require "digest/sha2"
require "tmpdir"

def noise(n, seed)
  x = seed
  out = []
  n.times do
    x = (x * 75 + 74) % 65537
    out << (x % 256).chr
  end
  out.join.b
end

VECTORS = [
  "",
  "abc",
  "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
  "a\0b\xffc".b,
]

SPLITS = [1, 7, 111, 112, 127, 128, 129, 257]

[Digest::SHA384, Digest::SHA512].each do |klass|
  VECTORS.each do |msg|
    hex = klass.hexdigest(msg)
    raw = klass.digest(msg)
    b64 = klass.base64digest(msg)
    puts "#{klass}: #{msg.bytesize} #{hex}"
    p [raw.bytesize, Digest.hexencode(raw) == hex, b64 == [raw].pack("m0")]
  end

  # Million 'a' NIST vector
  million = "a" * 1_000_000
  puts "#{klass} million: #{klass.hexdigest(million)}"

  # Every length across two 128-byte blocks (padding boundary at 111/112, 239/240)
  s = noise(260, 17)
  acc = klass.new
  (0..260).each do |n|
    part = s.byteslice(0, n)
    one = klass.digest(part)
    inc = klass.new.update(part).digest
    raise "mismatch at #{n}" unless one == inc
    acc << one
  end
  puts "#{klass} 0..260: #{acc.hexdigest}"

  # Multi-chunk boundary splits across 128-byte blocks
  msg = noise(420, 29) + "\0\0\xff".b + noise(140, 11)
  whole_hex = klass.hexdigest(msg)
  whole_raw = klass.digest(msg)
  splits_ok = SPLITS.all? do |step|
    d = klass.new
    i = 0
    while i < msg.bytesize
      d << msg.byteslice(i, step)
      i += step
    end
    d.hexdigest == whole_hex && d.digest == whole_raw && d.hexdigest == d.hexdigest
  end
  puts "#{klass} splits: #{splits_ok} #{whole_hex}"

  # Instance methods, dup/clone independence, reset, ==, ! forms, (data) forms
  d = klass.new
  p [d.digest_length, d.length, d.size, d.block_length]
  p d.inspect
  p d.to_s == d.hexdigest
  d << "hello "
  copy = d.dup
  cloned = d.clone
  copy << "world"
  cloned << "ruby"
  p [
    d.hexdigest == klass.hexdigest("hello "),
    copy.hexdigest == klass.hexdigest("hello world"),
    cloned.hexdigest == klass.hexdigest("hello ruby"),
  ]
  p [
    d == d.hexdigest,
    d == d.hexdigest.upcase,
    d == d.digest,
    d == klass.new.update("hello "),
    d == copy,
    d == nil,
    d == 42,
  ]
  p [d.digest("xyz") == klass.digest("xyz"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.hexdigest("xyz") == klass.hexdigest("xyz"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.base64digest("xyz") == klass.base64digest("xyz"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.digest! == klass.digest("abc"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.hexdigest! == klass.hexdigest("abc"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.base64digest! == klass.base64digest("abc"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.reset.hexdigest == klass.hexdigest(""), d.new.class == klass]
end

# Cross-algorithm inequality
p Digest::SHA384.new("".b) rescue p($!.class)
p Digest::SHA384.new == Digest::SHA512.new

# Digest::SHA2 wrapper
[256, 384, 512].each do |bits|
  d = Digest::SHA2.new(bits)
  d << "ab" << "c"
  copy = d.dup
  copy << "d"
  puts "SHA2(#{bits}): #{d.digest_length}/#{d.block_length} #{d.hexdigest} #{copy.hexdigest}"
  p d.inspect
end

# Streaming file hashing with Dir.mktmpdir
Dir.mktmpdir do |dir|
  path = File.join(dir, "payload.bin")
  data = noise(40_000, 5)
  File.binwrite(path, data)
  [Digest::SHA384, Digest::SHA512].each do |klass|
    f = klass.file(path)
    p [f.class, f.hexdigest == klass.hexdigest(data), f.update("tail").hexdigest == klass.hexdigest(data + "tail")]
  end
end
