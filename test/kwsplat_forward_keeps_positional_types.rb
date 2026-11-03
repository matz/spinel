# A `**h` argument binds only the keyword parameters it can fill
# `Enc.new(src, **opts)` typed every parameter of Enc#initialize from the
# Hash's value type, the positional `src` too: `src` read boxed, and so
# did every instance variable it was written to. A `**` fills keywords
# only; a callee that takes no keyword binds the Hash as one positional,
# as it always did.

class Img
  def initialize(w) = @w = w
  def width = @w
end

class Enc
  def initialize(src, scale, name, quality: 85, tag: "q")
    @src = src
    @scale = scale
    @name = name
    @quality = quality
    @tag = tag
  end

  def run = "#{@name}:#{@src.width * @scale}:#{@quality}:#{@tag}"
end

def enc(src, **opts) = Enc.new(src, 2, "e", **opts)

p enc(Img.new(3), quality: 5).run
p enc(Img.new(4)).run
p enc(Img.new(5), tag: "t", quality: 7).run

# a callee that takes no keyword: the Hash is one more positional
def pos(a, b = nil) = [a, b]
h = { k: 1 }
p pos(1, **h), pos(2, **{})

# beside a keyword rest
def kr(a, b = 0, **rest) = [a * 2, b, rest]
p kr(3, **h), kr(4, 5, **h)

# through a `...` forwarder (a class of its own: what `...` binds is
# another path's)
class Fwd
  def initialize(src, scale, quality: 1) = (@v = src.width * scale + quality)
  def v = @v
end
def fwd(...) = Fwd.new(...)
p fwd(Img.new(6), 3, quality: 9).v

# a keyword the Hash names, read beside a positional of another type
def mix(n, s, k: 0) = n + s.size + k
opts = { k: 10 }
p mix(1, "abc", **opts), mix(2, "de", **{})
