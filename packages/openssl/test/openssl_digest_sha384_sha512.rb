# OpenSSL::Digest SHA384/SHA512 streaming through lib/sp_crypto.c:
# NIST vectors, 128-byte block boundary splits, dup/clone independence, reset,
# file streaming, class methods, ! forms, HMAC streaming, and thread safety.
require "openssl"
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

[["SHA384", OpenSSL::Digest::SHA384], ["SHA512", OpenSSL::Digest::SHA512]].each do |name, klass|
  VECTORS.each do |msg|
    hex = klass.hexdigest(msg)
    raw = klass.digest(msg)
    b64 = klass.base64digest(msg)
    puts "#{name} #{msg.bytesize}: #{hex}"
    p [
      raw.bytesize,
      hex == OpenSSL::Digest.hexdigest(name, msg),
      raw == OpenSSL::Digest.digest(name, msg),
      b64 == OpenSSL::Digest.base64digest(name, msg),
      b64 == [raw].pack("m0"),
    ]
  end

  msg = noise(420, 19) + "\0\0\xff".b + noise(140, 7)
  whole = klass.hexdigest(msg)
  splits_ok = SPLITS.all? do |step|
    d = OpenSSL::Digest.new(name)
    i = 0
    while i < msg.bytesize
      d << msg.byteslice(i, step)
      i += step
    end
    d.hexdigest == whole && d.digest == klass.digest(msg) && d.hexdigest == d.hexdigest
  end
  puts "#{name} splits: #{splits_ok} #{whole}"

  d = klass.new
  p [d.name, d.digest_length, d.length, d.size, d.block_length]
  d << "prefix-"
  copy = d.dup
  cloned = d.clone
  copy << "dup"
  cloned << "clone"
  p [
    d.hexdigest == klass.hexdigest("prefix-"),
    copy.hexdigest == klass.hexdigest("prefix-dup"),
    cloned.hexdigest == klass.hexdigest("prefix-clone"),
    d == d.hexdigest,
    d == OpenSSL::Digest.new(name, "prefix-"),
    d == copy,
    d.to_s == d.hexdigest,
    d.inspect,
  ]
  p [d.digest! == klass.digest("prefix-"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.hexdigest! == klass.hexdigest("abc"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.base64digest! == klass.base64digest("abc"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.digest("x") == klass.digest("x"), d.hexdigest == klass.hexdigest("")]
  d << "abc"
  p [d.reset.hexdigest == klass.hexdigest(""), d.new.class == klass]

  # Streaming HMAC-SHA384 / HMAC-SHA512 with keyed dup
  [0, 32, 128, 131].each do |klen|
    key = noise(klen, 5)
    keyed = OpenSSL::HMAC.new(key, name)
    a = keyed.dup.update(msg.byteslice(0, 130)).update(msg.byteslice(130, msg.bytesize - 130))
    b = keyed.dup.update("other")
    p [
      a.hexdigest == OpenSSL::HMAC.hexdigest(name, key, msg),
      b.hexdigest == OpenSSL::HMAC.hexdigest(name, key, "other"),
      keyed.hexdigest == OpenSSL::HMAC.hexdigest(name, key, ""),
    ]
  end
end

# Streaming file hashing
Dir.mktmpdir do |dir|
  path = File.join(dir, "data.bin")
  data = noise(40_000, 13)
  File.binwrite(path, data)
  p OpenSSL::Digest::SHA384.file(path).hexdigest == OpenSSL::Digest::SHA384.hexdigest(data)
  p OpenSSL::Digest::SHA512.file(path).hexdigest == OpenSSL::Digest::SHA512.hexdigest(data)
  p OpenSSL::Digest.file(path, "SHA384").hexdigest == OpenSSL::Digest::SHA384.hexdigest(data)
  p OpenSSL::Digest.file(path, "SHA512").hexdigest == OpenSSL::Digest::SHA512.hexdigest(data)
end

# Multi-threaded TLS buffer isolation for SHA-384 and SHA-512
inputs = 32.times.map { |i| "thread-msg-#{i}-" + ("x" * (i * 11)) }
ref384 = inputs.map { |m| OpenSSL::Digest::SHA384.hexdigest(m) }
ref512 = inputs.map { |m| OpenSSL::Digest::SHA512.hexdigest(m) }
threads = 4.times.map do
  Thread.new do
    ok = true
    5.times do
      inputs.each_with_index do |m, i|
        ok = false if OpenSSL::Digest::SHA384.hexdigest(m) != ref384[i]
        ok = false if OpenSSL::Digest::SHA512.hexdigest(m) != ref512[i]
        d = OpenSSL::Digest::SHA512.new
        d << m.byteslice(0, 5) << m.byteslice(5, m.bytesize - 5)
        ok = false if d.hexdigest != ref512[i]
      end
    end
    ok
  end
end
p threads.all?(&:value)
