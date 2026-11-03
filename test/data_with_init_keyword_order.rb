# Data#with on a Data whose initialize declares its keywords in another
# order than the members builds the copy through that initialize, as CRuby
# does (`self.class.new(**to_h.merge(kw))`): each member reaches the keyword
# of its name. Handed over in member order, two Floats swapped, and a String
# or a Symbol beside a Float was a C type error.
Tween = Data.define(:from, :to, :elapsed)
class Tween
  def initialize(to:, from: 0.0, elapsed: 0.0)
    super(from: from.to_f, to: to.to_f, elapsed: elapsed.to_f)
  end

  def step(dt)
    with(elapsed: elapsed + dt.to_f)
  end

  def restart
    with(elapsed: 0.0)
  end
end

t = Tween.new(to: 1.0, from: 0.5)
t = t.step(0.3)
puts t.elapsed
puts t.from
puts t.restart.elapsed
p t.restart

Label = Data.define(:text, :x, :y, :kind)
class Label
  def initialize(x:, y:, kind: :plain, text: "")
    super(text: text.to_s, x: x.to_f, y: y.to_f, kind: kind)
  end
end
l = Label.new(x: 1.5, y: 2.5, text: "hi")
p l.with(y: 9.0)
p l.with(kind: :bold, text: "yo")

# initialize runs again for the copy; a keyword no member names takes its default
Box = Data.define(:w, :h)
class Box
  def initialize(h:, w:, scale: 2)
    super(w: w * scale, h: h * scale)
  end
end
b = Box.new(w: 1, h: 3)
p b, b.with(w: 5), b.with(scale: 1)

# a subclass's initialize, a member that holds nil, a **hash
Base = Data.define(:a, :b)
class Sub < Base
  def initialize(b:, a: nil)
    super(a: a, b: b)
  end
end
s = Sub.new(b: 2)
p s.with(b: 3), s.with(a: "y"), s.with(a: "y").with(b: nil)
h = {a: "z"}
p s.with(**h)

# CRuby's errors: a required keyword nothing fills, then a key initialize does not take
Req = Data.define(:a, :b)
class Req
  def initialize(b:, a:, k:)
    super(a: a, b: b + k)
  end
end
r = Req.new(a: 1, b: 2, k: 3)
p r.with(a: 2, k: 10)
begin; r.with(a: 2); rescue ArgumentError => e; p e.message; end
begin; r.with(k: 1, z: 2); rescue ArgumentError => e; p e.message; end

# every `**hash` and keyword in the call, the last one naming a keyword winning
h1 = {a: "s"}
h2 = {b: 5}
h3 = {b: 7}
p s.with(**h1, **h2), s.with(**h2, **h1), s.with(**h2, **h3)
p s.with(a: "k", **h1), s.with(**h1, a: "k"), s.with(b: 8, **h2)

# a nil reaches the member that super hands it to, not the one of its name
Swap = Data.define(:a, :b)
class Swap
  def initialize(a:, b:)
    super(a: b, b: a)
  end
end
w = Swap.new(a: 1, b: 2)
p w, w.with(a: nil), w.with(b: 5), Swap.new(a: nil, b: 3)
