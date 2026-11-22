# Digest::Base, Digest::Class, and Digest::Instance hierarchy and subclassing
# semantics matching CRuby 4.0.
require "digest"
require "tmpdir"

[Digest::MD5, Digest::SHA1, Digest::SHA256, Digest::SHA384, Digest::SHA512].each do |klass|
  d = klass.new
  p [
    klass.name,
    klass < Digest::Base,
    klass < Digest::Class,
    d.is_a?(Digest::Base),
    d.is_a?(Digest::Class),
    d.is_a?(Digest::Instance),
    d.digest_length,
    d.block_length,
  ]
end

begin
  Digest::Base.new
rescue NotImplementedError => e
  puts "#{e.class}: #{e.message}"
end

class DirectBaseSub < Digest::Base
end

begin
  DirectBaseSub.new
rescue RuntimeError => e
  puts "#{e.class}: #{e.message}"
end

# Subclassing a concrete Digest::Base subclass works as in CRuby
class TaggedSHA512 < Digest::SHA512
  def tag = "tagged"
end

t = TaggedSHA512.new
t << "ab"
t.update("c")
p [
  t.class,
  t.tag,
  t.is_a?(Digest::SHA512),
  t.is_a?(Digest::Base),
  t.hexdigest == Digest::SHA512.hexdigest("abc"),
  TaggedSHA512.hexdigest("abc") == Digest::SHA512.hexdigest("abc"),
  t.dup.update("d").hexdigest == Digest::SHA512.hexdigest("abcd"),
  t.hexdigest == Digest::SHA512.hexdigest("abc"),
]

# Subclassing Digest::Class in pure Ruby
class XorDigest < Digest::Class
  def initialize
    @state = [0, 0, 0, 0]
    @pos = 0
  end

  def initialize_copy(other)
    super
    @state = @state.dup
  end

  def update(str)
    str.each_byte do |b|
      @state[@pos & 3] ^= b
      @pos += 1
    end
    self
  end
  alias << update

  private def finish
    @state.pack("C4")
  end

  def reset
    @state = [0, 0, 0, 0]
    @pos = 0
    self
  end

  def digest_length = 4
  def block_length = 4
end

x = XorDigest.new
x << "hello "
y = x.dup
y << "world"
p [x.hexdigest, y.hexdigest, x.to_s, x.inspect]
p [x.digest.bytes, x.base64digest, x.digest_length, x.length, x.size, x.block_length]
p [x == x.hexdigest, x == XorDigest.new.update("hello "), x == y, x == nil]
p [XorDigest.hexdigest("hello world") == y.hexdigest, XorDigest.digest("hello world") == y.digest]
p [XorDigest.base64digest("hello world") == y.base64digest]
p [x.digest! == XorDigest.digest("hello "), x.hexdigest == XorDigest.hexdigest("")]
x << "abc"
p [x.hexdigest("xy") == XorDigest.hexdigest("xy"), x.hexdigest == XorDigest.hexdigest("")]
p x.new.class

Dir.mktmpdir do |dir|
  path = File.join(dir, "xor.bin")
  File.binwrite(path, "hello world")
  f = XorDigest.file(path)
  p [f.class, f.hexdigest == y.hexdigest]
end
